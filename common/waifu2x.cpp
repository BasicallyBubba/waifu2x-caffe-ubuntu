#include "waifu2x.h"
#include "stImage.h"
#include "cNet.h"
#include <caffe/caffe.hpp>
#include <cudnn.h>
#include <mutex>
#include <opencv2/core.hpp>
#include <tclap/CmdLine.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <chrono>
#include <cuda_runtime.h>

#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>

#include <fcntl.h>
#include <zlib.h>
#ifdef _MSC_VER
#include <io.h>
#endif

//#if defined(WIN32) || defined(WIN64)
//#include <Windows.h>
//#endif

#define CV_VERSION_STR CVAUX_STR(CV_MAJOR_VERSION) CVAUX_STR(CV_MINOR_VERSION) CVAUX_STR(CV_SUBMINOR_VERSION)

// �r���h���[�h
#ifdef _DEBUG
#define CV_EXT_STR "d.lib"
#else
#define CV_EXT_STR ".lib"
#endif

#ifdef _MSC_VER

#pragma comment(lib, "opencv_core" CV_VERSION_STR CV_EXT_STR)
#pragma comment(lib, "opencv_imgcodecs" CV_VERSION_STR CV_EXT_STR)
#pragma comment(lib, "opencv_imgproc" CV_VERSION_STR CV_EXT_STR)
//#pragma comment(lib, "IlmImf" CV_EXT_STR)
//#pragma comment(lib, "libjasper" CV_EXT_STR)
//#pragma comment(lib, "libjpeg" CV_EXT_STR)
//#pragma comment(lib, "libpng" CV_EXT_STR)
//#pragma comment(lib, "libtiff" CV_EXT_STR)
//#pragma comment(lib, "libwebp" CV_EXT_STR)

#pragma comment(lib, "libopenblas.dll.a")
#pragma comment(lib, "cudart.lib")
#pragma comment(lib, "curand.lib")
#pragma comment(lib, "cublas.lib")
#pragma comment(lib, "cudnn.lib")

#ifdef _DEBUG
#pragma comment(lib, "caffe-d.lib")
#pragma comment(lib, "proto-d.lib")
#pragma comment(lib, "libboost_system-vc120-mt-gd-1_59.lib")
#pragma comment(lib, "libboost_thread-vc120-mt-gd-1_59.lib")
#pragma comment(lib, "libboost_filesystem-vc120-mt-gd-1_59.lib")
#pragma comment(lib, "glogd.lib")
#pragma comment(lib, "gflagsd.lib")
#pragma comment(lib, "libprotobufd.lib")
#pragma comment(lib, "libhdf5_hl_D.lib")
#pragma comment(lib, "libhdf5_D.lib")
#pragma comment(lib, "zlibstaticd.lib")

#pragma comment(lib, "libboost_iostreams-vc120-mt-gd-1_59.lib")
#else
#pragma comment(lib, "caffe.lib")
#pragma comment(lib, "proto.lib")
#pragma comment(lib, "libboost_system-vc120-mt-1_59.lib")
#pragma comment(lib, "libboost_thread-vc120-mt-1_59.lib")
#pragma comment(lib, "libboost_filesystem-vc120-mt-1_59.lib")
#pragma comment(lib, "glog.lib")
#pragma comment(lib, "gflags.lib")
#pragma comment(lib, "libprotobuf.lib")
#pragma comment(lib, "libhdf5_hl.lib")
#pragma comment(lib, "libhdf5.lib")
#pragma comment(lib, "zlibstatic.lib")

#pragma comment(lib, "libboost_iostreams-vc120-mt-1_59.lib")
#endif
#endif

const int ScaleBase = 2; // TODO: ���f���̊g�嗦�ɂ���ĉςł���悤�ɂ���

// ���͉摜�ɒǉ�����p�f�B���O
const int OuterPadding = 0;

// �Œ���K�v��CUDA�h���C�o�[�̃o�[�W����
const int MinCudaDriverVersion = 7050;

static std::once_flag waifu2x_once_flag;
static std::once_flag waifu2x_cudnn_once_flag;
static std::once_flag waifu2x_cuda_once_flag;

std::string Waifu2x::ExeDir;


#ifndef CUDA_CHECK_WAIFU2X
#define CUDA_CHECK_WAIFU2X(condition) \
 do { \
    cudaError_t error = condition; \
    if(error != cudaSuccess) throw error; \
 } while (0)
#endif

#define CUDA_HOST_SAFE_FREE(ptr) \
	do { \
		if (ptr) { \
			cudaFreeHost(ptr); \
			ptr = nullptr; \
		} \
	} while (0)

#define SAFE_DELETE_WAIFU2X(ptr) \
	do { \
		if (ptr) { \
			delete [] ptr; \
			ptr = nullptr; \
		} \
	} while (0)

namespace
{
	class IgnoreErrorCV
	{
	private:
		static int handleError(int status, const char* func_name,
			const char* err_msg, const char* file_name,
			int line, void* userdata)
		{
			return 0;
		}

	public:
		IgnoreErrorCV()
		{
			cv::redirectError(handleError);
		}
	};

	IgnoreErrorCV g_IgnoreErrorCV;

	class CudaDeviceSet
	{
	private:
		int orgDevice;
		bool mIsSet;

	public:
		CudaDeviceSet(const std::string &process, const int devno) : orgDevice(0), mIsSet(false)
		{
			if (process == "gpu" || process == "cudnn")
			{
				int count = 0;
				if (cudaGetDeviceCount(&count) != CUDA_SUCCESS)
					return;

				if (devno >= count || count < 0)
					return;

				if (cudaGetDevice(&orgDevice) != CUDA_SUCCESS)
					return;

				if (cudaSetDevice(devno) != CUDA_SUCCESS)
					return;

				mIsSet = true;
			}
		}

		~CudaDeviceSet()
		{
			if (mIsSet)
				cudaSetDevice(orgDevice);
		}
	};
}

// CUDA���g���邩�`�F�b�N
Waifu2x::eWaifu2xCudaError Waifu2x::can_use_CUDA()
{
	static eWaifu2xCudaError CudaFlag = eWaifu2xCudaError_NotFind;
	std::call_once(waifu2x_cuda_once_flag, [&]()
	{
		int driverVersion = 0;
		if (cudaDriverGetVersion(&driverVersion) == cudaSuccess)
		{
			if (driverVersion > 0)
			{
				int runtimeVersion;
				if (cudaRuntimeGetVersion(&runtimeVersion) == cudaSuccess)
				{
					if (runtimeVersion >= MinCudaDriverVersion && driverVersion >= runtimeVersion)
					{
						cudaDeviceProp prop;
						cudaGetDeviceProperties(&prop, 0);
						if (prop.major >= 2)
							CudaFlag = eWaifu2xCudaError_OK;
						else
							CudaFlag = eWaifu2xCudaError_OldDevice;
					}
					else
						CudaFlag = eWaifu2xCudaError_OldVersion;
				}
				else
					CudaFlag = eWaifu2xCudaError_NotFind;
			}
			else
				CudaFlag = eWaifu2xCudaError_NotFind;
		}
		else
			CudaFlag = eWaifu2xCudaError_NotFind;
	});

	return CudaFlag;
}

// cuDNN���g���邩�`�F�b�N�B����Windows�̂�
Waifu2x::eWaifu2xcuDNNError Waifu2x::can_use_cuDNN()
{
	static eWaifu2xcuDNNError cuDNNFlag = eWaifu2xcuDNNError_NotFind;
	std::call_once(waifu2x_cudnn_once_flag, [&]()
	{
#if defined(WIN32) || defined(WIN64)
		HMODULE hModule = LoadLibrary(TEXT(CUDNN_DLL_NAME));
		if (hModule != NULL)
		{
			typedef cudnnStatus_t(__stdcall * cudnnCreateType)(cudnnHandle_t *);
			typedef cudnnStatus_t(__stdcall * cudnnDestroyType)(cudnnHandle_t);
			typedef uint64_t(__stdcall * cudnnGetVersionType)();

			cudnnCreateType cudnnCreateFunc = (cudnnCreateType)GetProcAddress(hModule, "cudnnCreate");
			cudnnDestroyType cudnnDestroyFunc = (cudnnDestroyType)GetProcAddress(hModule, "cudnnDestroy");
			cudnnGetVersionType cudnnGetVersionFunc = (cudnnGetVersionType)GetProcAddress(hModule, "cudnnGetVersion");
			if (cudnnCreateFunc != nullptr && cudnnDestroyFunc != nullptr && cudnnGetVersionFunc != nullptr)
			{
				if (cudnnGetVersionFunc() >= 3000)
				{
					cudnnHandle_t h;
					if (cudnnCreateFunc(&h) == CUDNN_STATUS_SUCCESS)
					{
						if (cudnnDestroyFunc(h) == CUDNN_STATUS_SUCCESS)
							cuDNNFlag = eWaifu2xcuDNNError_OK;
						else
							cuDNNFlag = eWaifu2xcuDNNError_CannotCreate;
					}
					else
						cuDNNFlag = eWaifu2xcuDNNError_CannotCreate;
				}
				else
					cuDNNFlag = eWaifu2xcuDNNError_OldVersion;
			}
			else
				cuDNNFlag = eWaifu2xcuDNNError_NotFind;

			FreeLibrary(hModule);
		}
#endif
	});

	return cuDNNFlag;
}

void Waifu2x::init_liblary(int argc, char** argv)
{
	if (argc > 0)
		ExeDir = argv[0];

	std::call_once(waifu2x_once_flag, [argc, argv]()
	{
		assert(argc >= 1);

		int tmpargc = 1;
		char* tmpargvv[] = {argv[0]};
		char** tmpargv = tmpargvv;
		// glog���̏�����
		caffe::GlobalInit(&tmpargc, &tmpargv);
	});
}

void Waifu2x::quit_liblary()
{}


Waifu2x::Waifu2x() : mIsInited(false), mNoiseLevel(0), mIsCuda(false), mOutputBlock(nullptr), mOutputBlockSize(0), mGPUNo(0)
{}

Waifu2x::~Waifu2x()
{
	Destroy();
}

Waifu2x::eWaifu2xError Waifu2x::Init(const eWaifu2xModelType mode, const int noise_level,
	const boost::filesystem::path &model_dir, const std::string &process, const int GPUNo)
{
	Waifu2x::eWaifu2xError ret;

	if (mIsInited)
		return Waifu2x::eWaifu2xError_OK;

	try
	{
		std::string Process = process;
		const auto cuDNNCheckStartTime = std::chrono::system_clock::now();

		if (Process == "gpu")
		{
			if (can_use_CUDA() != eWaifu2xCudaError_OK)
				return Waifu2x::eWaifu2xError_FailedCudaCheck;
			// cuDNN���g�������Ȃ�cuDNN���g��
			else if (can_use_cuDNN() == eWaifu2xcuDNNError_OK)
				Process = "cudnn";
		}

		mMode = mode;
		mNoiseLevel = noise_level;
		mProcess = Process;
		mGPUNo = GPUNo;

		const auto cuDNNCheckEndTime = std::chrono::system_clock::now();

		const boost::filesystem::path mode_dir_path(GetModeDirPath(model_dir));
		if (!boost::filesystem::exists(mode_dir_path))
			return Waifu2x::eWaifu2xError_FailedOpenModelFile;

		CudaDeviceSet devset(process, mGPUNo);

		if (mProcess == "cpu")
		{
			caffe::Caffe::set_mode(caffe::Caffe::CPU);
			mIsCuda = false;
		}
		else
		{
			caffe::Caffe::set_mode(caffe::Caffe::GPU);
			mIsCuda = true;
		}

		mInputPlane = 0;
		mMaxNetOffset = 0;

		const boost::filesystem::path info_path = GetInfoPath(mode_dir_path);

		stInfo info;
		ret = cNet::GetInfo(info_path, info);
		if (ret != Waifu2x::eWaifu2xError_OK)
			return ret;

		mHasNoiseScale = info.has_noise_scale;
		mInputPlane = info.channels;

		if (mode == eWaifu2xModelTypeNoise || mode == eWaifu2xModelTypeNoiseScale || mode == eWaifu2xModelTypeAutoScale)
		{
			std::string base_name;

			mNoiseNet.reset(new cNet);

			eWaifu2xModelType Mode = mode;
			if (info.has_noise_scale) // �m�C�Y�����Ɗg��𓯎��ɍs��
			{
				// �m�C�Y�����g��l�b�g�̍\�z��eWaifu2xModelTypeNoiseScale���w�肷��K�v������
				Mode = eWaifu2xModelTypeNoiseScale;
				base_name = "noise" + std::to_string(noise_level) + "_scale2.0x_model";
			}
			else // �m�C�Y��������
			{
				Mode = eWaifu2xModelTypeNoise;
				base_name = "noise" + std::to_string(noise_level) + "_model";
			}

			const boost::filesystem::path model_path = mode_dir_path / (base_name + ".prototxt");
			const boost::filesystem::path param_path = mode_dir_path / (base_name + ".json");

			ret = mNoiseNet->ConstractNet(Mode, model_path, param_path, info, mProcess);
			if (ret != Waifu2x::eWaifu2xError_OK)
				return ret;

			mMaxNetOffset = mNoiseNet->GetNetOffset();
		}

		// noise_scale�������Ă���ꍇ�̓��`�����l���̊g��̂��߂�mScaleNet���\�z����K�v������
		if (info.has_noise_scale || mode == eWaifu2xModelTypeScale || mode == eWaifu2xModelTypeNoiseScale || mode == eWaifu2xModelTypeAutoScale)
		{
			const std::string base_name = "scale2.0x_model";

			const boost::filesystem::path model_path = mode_dir_path / (base_name + ".prototxt");
			const boost::filesystem::path param_path = mode_dir_path / (base_name + ".json");

			mScaleNet.reset(new cNet);

			ret = mScaleNet->ConstractNet(eWaifu2xModelTypeScale, model_path, param_path, info, mProcess);
			if (ret != Waifu2x::eWaifu2xError_OK)
				return ret;

			assert(mInputPlane == 0 || mInputPlane == mScaleNet->GetInputPlane());

			mMaxNetOffset = std::max(mScaleNet->GetNetOffset(), mMaxNetOffset);
		}
		else
		{
			
		}

		mIsInited = true;
	}
	catch (...)
	{
		return Waifu2x::eWaifu2xError_InvalidParameter;
	}

	return Waifu2x::eWaifu2xError_OK;
}

boost::filesystem::path Waifu2x::GetModeDirPath(const boost::filesystem::path &model_dir)
{
	boost::filesystem::path mode_dir_path(model_dir);
	if (!mode_dir_path.is_absolute()) // model_dir�����΃p�X�Ȃ��΃p�X�ɒ���
	{
		// �܂��̓J�����g�f�B���N�g�����ɂ��邩�T��
		mode_dir_path = boost::filesystem::absolute(model_dir);
		if (!boost::filesystem::exists(mode_dir_path) && !ExeDir.empty()) // ����������argv[0]������s�t�@�C���̂���t�H���_�𐄒肵�A���̃t�H���_���ɂ��邩�T��
		{
			boost::filesystem::path a0(ExeDir);
			if (a0.is_absolute())
				mode_dir_path = a0.branch_path() / model_dir;
		}
	}

	return mode_dir_path;
}

boost::filesystem::path Waifu2x::GetInfoPath(const boost::filesystem::path &mode_dir_path)
{
	const boost::filesystem::path info_path = mode_dir_path / "info.json";

	return info_path;
}

Waifu2x::eWaifu2xError Waifu2x::waifu2x(const boost::filesystem::path &input_file, const boost::filesystem::path &output_file,
	const boost::optional<double> scale_ratio, const boost::optional<int> scale_width, const boost::optional<int> scale_height, 
	const waifu2xCancelFunc cancel_func, const int crop_w, const int crop_h,
	const boost::optional<int> output_quality, const int output_depth, const bool use_tta,
	const int batch_size)
{
	Waifu2x::eWaifu2xError ret;

	if (!mIsInited)
		return Waifu2x::eWaifu2xError_NotInitialized;

	stImage image;
	ret = image.Load(input_file);
	if (ret != Waifu2x::eWaifu2xError_OK)
		return ret;

	image.Preprocess(mInputPlane, mMaxNetOffset);

	const bool isReconstructNoise = mMode == eWaifu2xModelTypeNoise || mMode == eWaifu2xModelTypeNoiseScale || (mMode == eWaifu2xModelTypeAutoScale && image.RequestDenoise());
	const bool isReconstructScale = mMode == eWaifu2xModelTypeScale || mMode == eWaifu2xModelTypeNoiseScale || mMode == eWaifu2xModelTypeAutoScale;

	double Factor = CalcScaleRatio(scale_ratio, scale_width, scale_height, image);

	if (!isReconstructScale)
		Factor = 1.0;

	cv::Mat reconstruct_image;
	ret = ReconstructImage(Factor, crop_w, crop_h, use_tta, batch_size, isReconstructNoise, isReconstructScale, cancel_func, image);
	if (ret != Waifu2x::eWaifu2xError_OK)
		return ret;

	image.Postprocess(mInputPlane, Factor, output_depth);

	ret = image.Save(output_file, output_quality);
	if (ret != Waifu2x::eWaifu2xError_OK)
		return ret;

	return Waifu2x::eWaifu2xError_OK;
}

Waifu2x::eWaifu2xError Waifu2x::waifu2x(const double factor, const void* source, void* dest, const int width, const int height,
	const int in_channel, const int in_stride, const int out_channel, const int out_stride,
	const int crop_w, const int crop_h, const bool use_tta, const int batch_size)
{
	Waifu2x::eWaifu2xError ret;

	if (!mIsInited)
		return Waifu2x::eWaifu2xError_NotInitialized;

	stImage image;
	ret = image.Load(source, width, height, in_channel, in_stride);
	if (ret != Waifu2x::eWaifu2xError_OK)
		return ret;

	image.Preprocess(mInputPlane, mMaxNetOffset);

	const bool isReconstructNoise = mMode == eWaifu2xModelTypeNoise || mMode == eWaifu2xModelTypeNoiseScale;
	const bool isReconstructScale = mMode == eWaifu2xModelTypeScale || mMode == eWaifu2xModelTypeNoiseScale || mMode == eWaifu2xModelTypeAutoScale;

	double Factor = factor;
	if (!isReconstructScale)
		Factor = 1.0;

	cv::Mat reconstruct_image;
	ret = ReconstructImage(Factor, crop_w, crop_h, use_tta, batch_size, isReconstructNoise, isReconstructScale, nullptr, image);
	if (ret != Waifu2x::eWaifu2xError_OK)
		return ret;

	image.Postprocess(mInputPlane, Factor, 8);

	cv::Mat out_image = image.GetEndImage();
	image.Clear();

	// �o�͔z��֏�������
	{
		const auto width = out_image.size().width;
		const auto stride = out_image.step1();
		for (int i = 0; i < out_image.size().height; i++)
			memcpy((uint8_t *)dest + out_stride * i, out_image.data + stride * i, stride);
	}

	return Waifu2x::eWaifu2xError_OK;
}

double Waifu2x::CalcScaleRatio(const boost::optional<double> scale_ratio, const boost::optional<int> scale_width, const boost::optional<int> scale_height,
	const stImage &image)
{
	if (scale_ratio)
		return *scale_ratio;

	if (scale_width)
		return image.GetScaleFromWidth(*scale_width);

	if(scale_height)
		return image.GetScaleFromWidth(*scale_height);

	return 1.0;
}

Waifu2x::eWaifu2xError Waifu2x::ReconstructImage(const double factor, const int crop_w, const int crop_h, const bool use_tta, const int batch_size,
	const bool isReconstructNoise, const bool isReconstructScale, const Waifu2x::waifu2xCancelFunc cancel_func, stImage &image)
{
	Waifu2x::eWaifu2xError ret;

	double Factor = factor;

	if (isReconstructNoise)
	{
		if (!mHasNoiseScale) // �m�C�Y��������
		{
			cv::Mat im;
			cv::Size_<int> size;
			image.GetScalePaddingedRGB(im, size, mNoiseNet->GetNetOffset(), OuterPadding, crop_w, crop_h, 1);

			ret = ProcessNet(mNoiseNet, crop_w, crop_h, use_tta, batch_size, im);
			if (ret != Waifu2x::eWaifu2xError_OK)
				return ret;

			image.SetReconstructedRGB(im, size, 1);
		}
		else // �m�C�Y�����Ɗg��
		{
			ret = ReconstructNoiseScale(crop_w, crop_h, use_tta, batch_size, cancel_func, image);
			if (ret != Waifu2x::eWaifu2xError_OK)
				return ret;

			Factor /= mNoiseNet->GetInnerScale();
		}
	}

	if (cancel_func && cancel_func())
		return Waifu2x::eWaifu2xError_Cancel;

	const int scaleNum = ceil(log(Factor) / log(ScaleBase));

	if (isReconstructScale)
	{
		for (int i = 0; i < scaleNum; i++)
		{
			ret = ReconstructScale(crop_w, crop_h, use_tta, batch_size, cancel_func, image);
			if (ret != Waifu2x::eWaifu2xError_OK)
				return ret;
		}
	}

	return Waifu2x::eWaifu2xError_OK;
}

Waifu2x::eWaifu2xError Waifu2x::ReconstructScale(const int crop_w, const int crop_h, const bool use_tta, const int batch_size,
	const Waifu2x::waifu2xCancelFunc cancel_func, stImage &image)
{
	Waifu2x::eWaifu2xError ret;

	if (image.HasAlpha())
	{
		cv::Mat im;
		cv::Size_<int> size;
		image.GetScalePaddingedA(im, size, mScaleNet->GetNetOffset(), OuterPadding, crop_w, crop_h, mScaleNet->GetScale() / mScaleNet->GetInnerScale());

		ret = ReconstructByNet(mScaleNet, crop_w, crop_h, use_tta, batch_size, cancel_func, im);
		if (ret != Waifu2x::eWaifu2xError_OK)
			return ret;

		image.SetReconstructedA(im, size, mScaleNet->GetInnerScale());
	}

	cv::Mat im;
	cv::Size_<int> size;
	image.GetScalePaddingedRGB(im, size, mScaleNet->GetNetOffset(), OuterPadding, crop_w, crop_h, mScaleNet->GetScale() / mScaleNet->GetInnerScale());

	ret = ReconstructByNet(mScaleNet, crop_w, crop_h, use_tta, batch_size, cancel_func, im);
	if (ret != Waifu2x::eWaifu2xError_OK)
		return ret;

	image.SetReconstructedRGB(im, size, mScaleNet->GetInnerScale());

	return Waifu2x::eWaifu2xError_OK;
}

Waifu2x::eWaifu2xError Waifu2x::ReconstructNoiseScale(const int crop_w, const int crop_h, const bool use_tta, const int batch_size,
	const Waifu2x::waifu2xCancelFunc cancel_func, stImage &image)
{
	Waifu2x::eWaifu2xError ret;

	if (image.HasAlpha())
	{
		// ���`�����l���ɂ̓m�C�Y�������s��Ȃ�

		cv::Mat im;
		cv::Size_<int> size;
		image.GetScalePaddingedA(im, size, mScaleNet->GetNetOffset(), OuterPadding, crop_w, crop_h, mScaleNet->GetScale() / mScaleNet->GetInnerScale());

		ret = ReconstructByNet(mScaleNet, crop_w, crop_h, use_tta, batch_size, cancel_func, im);
		if (ret != Waifu2x::eWaifu2xError_OK)
			return ret;

		image.SetReconstructedA(im, size, mScaleNet->GetInnerScale());
	}

	cv::Mat im;
	cv::Size_<int> size;
	image.GetScalePaddingedRGB(im, size, mNoiseNet->GetNetOffset(), OuterPadding, crop_w, crop_h, mNoiseNet->GetScale() / mNoiseNet->GetInnerScale());

	ret = ReconstructByNet(mNoiseNet, crop_w, crop_h, use_tta, batch_size, cancel_func, im);
	if (ret != Waifu2x::eWaifu2xError_OK)
		return ret;

	image.SetReconstructedRGB(im, size, mNoiseNet->GetInnerScale());

	return Waifu2x::eWaifu2xError_OK;
}


Waifu2x::eWaifu2xError Waifu2x::ReconstructByNet(std::shared_ptr<cNet> net, const int crop_w, const int crop_h, const bool use_tta, const int batch_size,
	const Waifu2x::waifu2xCancelFunc cancel_func, cv::Mat &im)
{
	Waifu2x::eWaifu2xError ret;

	if (!use_tta) // ���ʂɏ���
	{
		ret = ProcessNet(net, crop_w, crop_h, use_tta, batch_size, im);
		if (ret != Waifu2x::eWaifu2xError_OK)
			return ret;
	}
	else // Test-Time Augmentation Mode
	{
		const auto RotateClockwise90 = [](cv::Mat &mat)
		{
			cv::transpose(mat, mat);
			cv::flip(mat, mat, 1);
		};

		const auto RotateClockwise90N = [RotateClockwise90](cv::Mat &mat, const int rotateNum)
		{
			for (int i = 0; i < rotateNum; i++)
				RotateClockwise90(mat);
		};

		const auto RotateCounterclockwise90 = [](cv::Mat &mat)
		{
			cv::transpose(mat, mat);
			cv::flip(mat, mat, 0);
		};

		const auto RotateCounterclockwise90N = [RotateCounterclockwise90](cv::Mat &mat, const int rotateNum)
		{
			for (int i = 0; i < rotateNum; i++)
				RotateCounterclockwise90(mat);
		};

		cv::Mat reconstruct_image;
		for (int i = 0; i < 8; i++)
		{
			cv::Mat in(im.clone());

			const int rotateNum = i % 4;
			RotateClockwise90N(in, rotateNum);

			if (i >= 4)
				cv::flip(in, in, 1); // ���������]

			ret = ProcessNet(net, crop_w, crop_h, use_tta, batch_size, in);
			if (ret != Waifu2x::eWaifu2xError_OK)
				return ret;

			if (i >= 4)
				cv::flip(in, in, 1); // ���������]

			RotateCounterclockwise90N(in, rotateNum);

			if (i == 0)
				reconstruct_image = in;
			else
				reconstruct_image += in;
		}

		reconstruct_image /= 8.0;

		im = reconstruct_image;
	}

	return Waifu2x::eWaifu2xError_OK;
}

Waifu2x::eWaifu2xError Waifu2x::ProcessNet(std::shared_ptr<cNet> net, const int crop_w, const int crop_h, const bool use_tta, const int batch_size, cv::Mat &im)
{
	Waifu2x::eWaifu2xError ret;

	CudaDeviceSet devset(mProcess, mGPUNo);

	const auto OutputMemorySize = net->GetOutputMemorySize(crop_w, crop_h, OuterPadding, batch_size);
	if (OutputMemorySize > mOutputBlockSize)
	{
		if (mIsCuda)
			CUDA_HOST_SAFE_FREE(mOutputBlock);
		else
			SAFE_DELETE_WAIFU2X(mOutputBlock);

		CUDA_CHECK_WAIFU2X(cudaHostAlloc(&mOutputBlock, OutputMemorySize, cudaHostAllocDefault));

		mOutputBlockSize = OutputMemorySize;
	}

	ret = net->ReconstructImage(use_tta, crop_w, crop_h, OuterPadding, batch_size, mOutputBlock, im, im);
	if (ret != Waifu2x::eWaifu2xError_OK)
		return ret;

	return Waifu2x::eWaifu2xError_OK;
}

void Waifu2x::Destroy()
{
	CudaDeviceSet devset(mProcess, mGPUNo);

	mNoiseNet.reset();
	mScaleNet.reset();

	if (mIsCuda)
	{
		CUDA_HOST_SAFE_FREE(mOutputBlock);
	}
	else
	{
		SAFE_DELETE_WAIFU2X(mOutputBlock);
	}

	mIsInited = false;
}

const std::string& Waifu2x::used_process() const
{
	return mProcess;
}

std::string Waifu2x::GetModelName(const boost::filesystem::path & model_dir)
{
	const boost::filesystem::path mode_dir_path(GetModeDirPath(model_dir));
	if (!boost::filesystem::exists(mode_dir_path))
		return std::string();

	const boost::filesystem::path info_path = mode_dir_path / "info.json";

	return cNet::GetModelName(info_path);
}
