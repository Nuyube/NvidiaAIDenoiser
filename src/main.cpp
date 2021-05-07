#include <optix_world.h>
#include <vector>
#include <iostream>
#include <fstream>
#include <iomanip>
#include "OpenImageIO\imageio.h"
#include "OpenImageIO\imagebuf.h"
#include <stdio.h>
#include <exception>

#include <time.h>
#ifdef _WIN32
#include <thread>
#include <chrono>
#include <windows.h>
#include <winternl.h>
#endif
#define MULTITHREADED_SAVING
#define MAX_CONCURRENT_THREADS 16

#define DENOISER_MAJOR_VERSION 2
#define DENOISER_MINOR_VERSION 5

#ifdef _WIN32
int getSysOpType()
{
	int ret = 0;
	NTSTATUS(WINAPI * RtlGetVersion)(LPOSVERSIONINFOEXW);
	OSVERSIONINFOEXW osInfo;

	*(FARPROC*)&RtlGetVersion = GetProcAddress(GetModuleHandleA("ntdll"), "RtlGetVersion");

	if (NULL != RtlGetVersion)
	{
		osInfo.dwOSVersionInfoSize = sizeof(osInfo);
		RtlGetVersion(&osInfo);
		ret = osInfo.dwMajorVersion;
	}
	return ret;
}
#endif

void exitfunc(int exit_code)
{
#ifdef _WIN32
	if (getSysOpType() < 10)
	{
		HANDLE tmpHandle = OpenProcess(PROCESS_ALL_ACCESS, TRUE, GetCurrentProcessId());
		if (tmpHandle != NULL)
		{
			std::cout << "terminating..." << std::endl;
			std::this_thread::sleep_for(std::chrono::seconds(1)); // delay 1s
			TerminateProcess(tmpHandle, 0);
		}
	}
#endif
	std::cout << "Exiting with code " << exit_code << "." << std::endl;
	exit(exit_code);
}

struct job {
	//Input components
	std::string beauty_path, normal_path, albedo_path;

	//Output components
	std::string output_path;

	//Image buffers
	OIIO::ImageBuf* beauty = nullptr, * albedo = nullptr, * normal = nullptr;

	bool beauty_loaded = false, albedo_loaded = false, normal_loaded = false;
	bool multithreaded_operation = false;
	void clean() {
		try {
			if (beauty) delete beauty;
			if (albedo) delete albedo;
			if (normal) delete normal;
		}
		catch (const std::exception& e) {
			std::cerr << "Exception cleaning job: " << e.what() << std::endl;
		}
	}

	bool valid() {
		return !beauty_path.empty() && !output_path.empty();
	}

	void load_files() {
		//Beauty is requisite
		loadfile(&beauty, beauty_path, beauty_loaded);

		if (!normal_path.empty())
			loadfile(&normal, normal_path, normal_loaded);
		if (!albedo_path.empty())
			loadfile(&albedo, albedo_path, albedo_loaded);
	}

private:
	void loadfile(OIIO::ImageBuf** buffer, std::string file, bool& loaded_flag) {
		(*buffer) = new OIIO::ImageBuf(file);
		if ((*buffer)->init_spec(file, 0, 0)) {
			std::cout << "Loaded successfully" << std::endl;
			loaded_flag = true;
		}
		else
		{
			std::cout << "Failed to load input image" << std::endl;
			std::cout << "[OIIO]: " << (*buffer)->geterror() << std::endl;
			clean();
			exitfunc(EXIT_FAILURE);
		}
	}
};

void printParams()
{
	std::cout << "Command line parameters" << std::endl;
	std::cout << "-i [string]   : path to input image" << std::endl;
	std::cout << "-f [string]   : path to input/output text file" << std::endl;
	std::cout << "              : file is in simple input path > output path format" << std::endl;
	std::cout << "-m            : multithread file saving. Useful if you're denoising large images." << std::endl;
	std::cout << "              : Note that this feature is experimental, and may break. Back up your data just in case!" << std::endl;
	std::cout << "-o [string]   : path to output image" << std::endl;
	std::cout << "-a [string]   : path to input albedo AOV (optional)" << std::endl;
	std::cout << "-n [string]   : path to input normal AOV (optional, requires albedo AOV)" << std::endl;
	std::cout << "-b [float]    : blend amount (default 0)" << std::endl;
	std::cout << "-hdr [int]    : Use HDR training data (default 1)" << std::endl;
	std::cout << "-maxmem [int] : Maximum memory size used by the denoiser in MB" << std::endl;
	std::cout << "-repeat [int] : Execute the denoiser N times. Useful for profiling." << std::endl;
}

int main(int argc, char* argv[])
{
	std::cout << "Launching Nvidia AI Denoiser command line app v" << DENOISER_MAJOR_VERSION << "." << DENOISER_MINOR_VERSION << std::endl;
	std::cout << "Created by Declan Russell (25/12/2017 ~ Merry Christmas!)" << std::endl;

	bool b_loaded, n_loaded, a_loaded;
	b_loaded = n_loaded = a_loaded = false;

	// For our input/output pair file
	bool file_input_output_list = false;
	std::string iopair_path;
#ifdef MULTITHREADED_SAVING
	std::vector<std::thread> saving_threads;
#endif
	std::vector<job> tasks;
	job cli_job;
	// Pass our command line args
	std::string out_path;
	float blend = 0.f;
	unsigned int hdr = 1;
	unsigned int num_runs = 1;
	float maxmem = 0.f;
	if (argc == 1)
	{
		printParams();
		exitfunc(EXIT_SUCCESS);
	}
	for (int i = 1; i < argc; i++)
	{
		const std::string arg(argv[i]);
		if (arg == "-i")
		{
			i++;
			std::string path(argv[i]);
			std::cout << "Input image: " << path << std::endl;
			cli_job.beauty_path = path;
		}
		else if (arg == "-f")
		{
			i++;
			iopair_path = std::string(argv[i]);
			file_input_output_list = true;
			std::cout << "Input i/o pair file: " << iopair_path << std::endl;
		}
		else if (arg == "-n")
		{
			i++;
			std::string path(argv[i]);
			std::cout << "Normal image: " << path << std::endl;
			cli_job.normal_path = path;
		}
		else if (arg == "-a")
		{
			i++;
			std::string path(argv[i]);
			std::cout << "Albedo image: " << path << std::endl;
			cli_job.albedo_path = path;
		}
		else if (arg == "-o")
		{
			i++;
			out_path = std::string(argv[i]);
			cli_job.output_path = out_path;
			std::cout << "Output image: " << out_path << std::endl;
		}
		else if (arg == "-b")
		{
			i++;
			std::string blend_string(argv[i]);
			blend = std::stof(blend_string);
			std::cout << "Blend amount: " << blend << std::endl;
		}
		else if (arg == "-hdr")
		{
			i++;
			std::string hdr_string(argv[i]);
			hdr = std::stoi(hdr_string);
			std::cout << ((hdr) ? "HDR training data enabled" : "HDR training data disabled") << std::endl;
		}
		else if (arg == "-maxmem")
		{
			i++;
			std::string maxmem_string(argv[i]);
			maxmem = float(std::stoi(maxmem_string) * 1ULL << 20);
			std::cout << "Maximum denoiser memory set to " << maxmem << std::endl;
		}
		else if (arg == "-repeat")
		{
			i++;
			std::string repeat_string(argv[i]);
			num_runs = std::max(std::stoi(repeat_string), 1);
			std::cout << "Number of repeats set to " << num_runs << std::endl;
		}
		else if (arg == "-h" || arg == "--help")
		{
			printParams();
		}
	}

	if (cli_job.valid()) {
		tasks.push_back(cli_job);
	}
	//If we're using a pairs file, read it.
	if (file_input_output_list) {
		std::cout << "Reading input pairs file at " << iopair_path << std::endl;
		std::ifstream pairsFile(iopair_path);
		if (pairsFile.fail()) {
			std::cerr << "Failed to open the pairs file." << std::endl;
			exitfunc(EXIT_FAILURE);
		}
		//Large enough to hold two files of MAX_PATH size plus some whitespace.
		char buffer[512];
		while (pairsFile.getline(buffer, 512)) {
			std::string wholeLine(buffer);
			if (wholeLine.empty())break;
			//Find the colon
			int idxColon = wholeLine.find('>');
			if (idxColon == -1) {
				//No colon = no service
				std::cerr << "Invalid line in jobs file: no > between input and output files" << std::endl;
				std::cout << wholeLine << std::endl;
				continue;
			}
			else {
				std::string input = wholeLine.substr(0, idxColon);
				std::string output = wholeLine.substr(idxColon + 1);
				job j = job();
				j.beauty_path = input;
				j.output_path = output;
				tasks.push_back(j);
				std::cout << "Read job with input " << input << " and output " << output << "." << std::endl;
			}
		}
		pairsFile.close();
	}

	for (int i = 0; i < tasks.size(); i++) {
		job this_task;
		try {
			this_task = tasks.at(i);
			this_task.load_files();
		}
		catch (const std::exception& e) {
			std::cerr << "Error reading files: " << e.what() << std::endl;
			continue;
		}
		// Check if a beauty has been loaded
		if (!this_task.beauty_loaded)
		{
			std::cerr << "No input image could be loaded" << std::endl;
			//We only need to clean this task, since the others should already be clean.
			this_task.clean();
			exitfunc(EXIT_FAILURE);
		}

		// If a normal AOV is loaded then we also require an albedo AOV
		if (this_task.normal_loaded && !this_task.albedo_loaded)
		{
			std::cerr << "You cannot use a normal AOV without an albedo" << std::endl;
			//We only need to clean this task, since the others should already be clean.
			this_task.clean();
			exitfunc(EXIT_FAILURE);
		}

		// Check for a file extension
		int x = (int)this_task.output_path.find_last_of(".");
		x++;
		const char* ext_c = this_task.output_path.c_str() + x;
		std::string ext(ext_c);
		if (!ext.size())
		{
			std::cerr << "No output file extension" << std::endl;
			//We only need to clean this task, since the others should already be clean.
			this_task.clean();
			exitfunc(EXIT_FAILURE);
		}
		int b_width, b_height, a_width, a_height, n_width, n_height;
		OIIO::ROI beauty_roi, albedo_roi, normal_roi;
		try {
			beauty_roi = OIIO::get_roi_full(this_task.beauty->spec());
			b_width = beauty_roi.width();
			b_height = beauty_roi.height();
			if (this_task.albedo_loaded)
			{
				albedo_roi = OIIO::get_roi_full(this_task.albedo->spec());
				if (this_task.normal_loaded)
					normal_roi = OIIO::get_roi_full(this_task.normal->spec());
			}

			// Check that our feature buffers are the same resolution as our beauty
			a_width = (this_task.albedo_loaded) ? albedo_roi.width() : 0;
			a_height = (this_task.albedo_loaded) ? albedo_roi.height() : 0;
			if (this_task.albedo_loaded)
			{
				if (a_width != b_width || a_height != b_height)
				{
					std::cerr << "Aldedo image not same resolution as beauty" << std::endl;
					//We only need to clean this task, since the others should already be clean.
					this_task.clean();
					exitfunc(EXIT_FAILURE);
				}
			}

			n_width = (this_task.normal_loaded) ? normal_roi.width() : 0;
			n_height = (this_task.normal_loaded) ? normal_roi.height() : 0;
			if (this_task.normal_loaded)
			{
				if (n_width != b_width || n_height != b_height)
				{
					std::cerr << "Normal image not same resolution as beauty" << std::endl;
					//We only need to clean this task, since the others should already be clean.
					this_task.clean();
					exitfunc(EXIT_FAILURE);
				}
			}
		}
		catch (const std::exception& e) {
			std::cerr << "Exception in ROI assignment: " << e.what() << std::endl;
			continue;
		}
		// Get our pixel data
		std::vector<float> beauty_pixels(b_width * b_height * beauty_roi.nchannels());
		this_task.beauty->get_pixels(beauty_roi, OIIO::TypeDesc::FLOAT, &beauty_pixels[0]);
		// Catch optix exceptions
		try
		{
			// Create our optix context and image buffers
			optix::Context optix_context = optix::Context::create();
			optix::Buffer beauty_buffer = optix_context->createBuffer(RT_BUFFER_INPUT_OUTPUT, RT_FORMAT_FLOAT4, b_width, b_height);
			optix::Buffer albedo_buffer = optix_context->createBuffer(RT_BUFFER_INPUT_OUTPUT, RT_FORMAT_FLOAT4, a_width, a_height);
			optix::Buffer normal_buffer = optix_context->createBuffer(RT_BUFFER_INPUT_OUTPUT, RT_FORMAT_FLOAT4, n_width, n_height);
			optix::Buffer out_buffer = optix_context->createBuffer(RT_BUFFER_INPUT_OUTPUT, RT_FORMAT_FLOAT4, b_width, b_height);

			float* device_ptr = (float*)beauty_buffer->map();
			unsigned int pixel_idx = 0;
			for (unsigned int y = 0; y < b_height; y++)
				for (unsigned int x = 0; x < b_width; x++)
				{
					memcpy(device_ptr, &beauty_pixels[pixel_idx], sizeof(float) * beauty_roi.nchannels());
					device_ptr += 4;
					pixel_idx += beauty_roi.nchannels();
				}
			beauty_buffer->unmap();
			device_ptr = 0;

			if (this_task.albedo_loaded)
			{
				std::vector<float> albedo_pixels(a_width * a_height * albedo_roi.nchannels());
				this_task.albedo->get_pixels(albedo_roi, OIIO::TypeDesc::FLOAT, &albedo_pixels[0]);

				device_ptr = (float*)albedo_buffer->map();
				pixel_idx = 0;
				for (unsigned int y = 0; y < a_height; y++)
					for (unsigned int x = 0; x < a_width; x++)
					{
						memcpy(device_ptr, &albedo_pixels[pixel_idx], sizeof(float) * albedo_roi.nchannels());
						device_ptr += 4;
						pixel_idx += albedo_roi.nchannels();
					}
				albedo_buffer->unmap();
				device_ptr = 0;
			}

			if (this_task.normal_loaded)
			{
				std::vector<float> normal_pixels(n_width * n_height * normal_roi.nchannels());
				this_task.normal->get_pixels(normal_roi, OIIO::TypeDesc::FLOAT, &normal_pixels[0]);

				device_ptr = (float*)normal_buffer->map();
				pixel_idx = 0;
				for (unsigned int y = 0; y < n_height; y++)
					for (unsigned int x = 0; x < n_width; x++)
					{
						memcpy(device_ptr, &normal_pixels[pixel_idx], sizeof(float) * normal_roi.nchannels());
						device_ptr += 4;
						pixel_idx += normal_roi.nchannels();
					}
				normal_buffer->unmap();
				device_ptr = 0;
			}

			// Setup the optix denoiser post processing stage
			optix::PostprocessingStage denoiserStage = optix_context->createBuiltinPostProcessingStage("DLDenoiser");
			denoiserStage->declareVariable("input_buffer")->set(beauty_buffer);
			denoiserStage->declareVariable("output_buffer")->set(out_buffer);
			denoiserStage->declareVariable("blend")->setFloat(blend);
			denoiserStage->declareVariable("hdr")->setUint(hdr);
			if (maxmem) denoiserStage->declareVariable("maxmem")->setUint(maxmem);
			denoiserStage->declareVariable("input_albedo_buffer")->set(albedo_buffer);
			denoiserStage->declareVariable("input_normal_buffer")->set(normal_buffer);

			// Add the denoiser to the new optix command list
			optix::CommandList commandList = optix_context->createCommandList();
			commandList->appendPostprocessingStage(denoiserStage, b_width, b_height);
			commandList->finalize();

			// Compile our context. I'm not sure if this is needed given there is no megakernal?
			optix_context->validate();
			optix_context->compile();

			// Execute denoise
			int sum = 0;
			for (unsigned int i = 0; i < num_runs; i++)
			{
				std::cout << "Denoising..." << std::endl;
				clock_t start = clock(), diff;
				commandList->execute();
				diff = clock() - start;
				int msec = diff * 1000 / CLOCKS_PER_SEC;
				if (num_runs > 1)
					std::cout << "Denoising run " << i << " complete in " << msec / 1000 << "." << std::setfill('0') << std::setw(3) << msec % 1000 << " seconds" << std::endl;
				else
					std::cout << "Denoising complete in " << msec / 1000 << "." << std::setfill('0') << std::setw(3) << msec % 1000 << " seconds" << std::endl;
				sum += msec;
			}
			if (num_runs > 1)
			{
				sum /= num_runs;
				std::cout << "Denoising avg of " << num_runs << " complete in " << sum / 1000 << "." << std::setfill('0') << std::setw(3) << sum % 1000 << " seconds" << std::endl;
			}

			// Copy denoised image back to the cpu
			device_ptr = (float*)out_buffer->map();
			pixel_idx = 0;
			for (unsigned int y = 0; y < b_height; y++)
				for (unsigned int x = 0; x < b_width; x++)
				{
					memcpy(&beauty_pixels[pixel_idx], device_ptr, sizeof(float) * beauty_roi.nchannels());
					device_ptr += 4;
					pixel_idx += beauty_roi.nchannels();
				}
			out_buffer->unmap();
			device_ptr = 0;

			// Remove our gpu buffers
			beauty_buffer->destroy();
			normal_buffer->destroy();
			albedo_buffer->destroy();
			out_buffer->destroy();
			optix_context->destroy();
		}
		catch (const std::exception& e)
		{
			std::cerr << "[OptiX]: " << e.what() << std::endl;
			//We only need to clean this task, since the others should already be clean.
			this_task.clean();
			exitfunc(EXIT_FAILURE);
		}

		// If the image already exists delete it
		remove(this_task.output_path.c_str());

		// Set our OIIO pixels
		if (!this_task.beauty->set_pixels(beauty_roi, OIIO::TypeDesc::FLOAT, &beauty_pixels[0]))
			std::cerr << "Something went wrong setting pixels" << std::endl;
#ifndef MULTITHREADED_SAVING
		// Save the output image
		std::cout << "Saving to: " << this_task.output_path << std::endl;
		if (this_task.beauty->write(this_task.output_path))
			std::cout << "Done!" << std::endl;
		else
		{
			std::cerr << "Could not save file " << this_task.output_path << std::endl;
			std::cerr << "[OIIO]: " << this_task.beauty->geterror() << std::endl;
	}
		this_task.clean();
#else
		if (saving_threads.size() > MAX_CONCURRENT_THREADS) {
			try {
				saving_threads.at(0).join();
				saving_threads.erase(saving_threads.begin());
			}
			catch (const std::exception& e) {
				std::cerr << e.what() << std::endl;
			}
		}
		//Clean our other maps, if any.
		if (this_task.normal) delete this_task.normal;
		if (this_task.albedo) delete this_task.albedo;
		saving_threads.push_back(std::thread([&]() {
			try {
				//Copy the data for the beauty to a new buffer to avoid accessing a deleted object
				OIIO::ImageBuf* movedPtr = this_task.beauty;
				std::string output_copy = this_task.output_path;
				std::cout << "Saving image " << this_task.output_path << std::endl;
				if (this_task.beauty->write(this_task.output_path))
					std::cout << "Done!" << std::endl;
				else
				{
					std::cerr << "Could not save file " << this_task.output_path << std::endl;
					std::cerr << "[OIIO]: " << this_task.beauty->geterror() << std::endl;
				}
				delete movedPtr;
			}
			catch (const std::exception& e) {
				std::cerr << e.what() << std::endl;
			}
			}));
		//Don't clean the task since that crashes the program,
		//the lambda will delete the data for the image.
#endif
}
	for (int i = 0; i < saving_threads.size(); i++) {
		saving_threads.at(i).join();
	}
	std::cout << "All tasks finished!" << std::endl;

	exitfunc(EXIT_SUCCESS);
}