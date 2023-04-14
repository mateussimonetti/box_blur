#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <array>
#include <cassert>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

using namespace std;

//stds
mutex m;
condition_variable space_available;
condition_variable data_available;


// CONSTANTS
static const string INPUT_DIRECTORY = "../input";
static const string OUTPUT_DIRECTORY = "../output";
static const int FILTER_SIZE = 5;
static const int NUM_CHANNELS = 3;
static const unsigned NUM_PRODUCERS = 1;
static const unsigned NUM_CONSUMERS = 10;


class buffer {
    private:
        string content[1000];
        int in = 0; 
        int out = 0;
    public:
        const unsigned BUFFER_SIZE = 1000;
        unsigned counter = 0; 

        void add(string i) {
            content[in] = i;
            in = (in+1) % BUFFER_SIZE;
            counter++;
        }

        string get() {
            string v;
            v = content[out];
            out = (out+1) % BUFFER_SIZE;
            counter--;
            return v;
        }

        bool contains(string s) {
            return std::find(std::begin(content), std::end(content), s) != std::end(content);
        }
};

buffer imagesQueue, outputImages;

static const unsigned SLEEP_TIME = 0; // ms
typedef vector<vector<uint8_t>> single_channel_image_t;
typedef array<single_channel_image_t, NUM_CHANNELS> image_t;

image_t load_image(const string &filename)
{
    int width, height, channels;

    unsigned char *data = stbi_load(filename.c_str(), &width, &height, &channels, 0);
    if (!data)
    {
        throw runtime_error("Failed to load image " + filename);
    }

    image_t result;
    for (int i = 0; i < NUM_CHANNELS; ++i)
    {
        result[i] = single_channel_image_t(height, vector<uint8_t>(width));
    }

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            for (int c = 0; c < NUM_CHANNELS; ++c)
            {
                result[c][y][x] = data[(y * width + x) * NUM_CHANNELS + c];
            }
        }
    }
    stbi_image_free(data);
    return result;
}

void write_image(const string &filename, const image_t &image)
{
    int channels = image.size();
    int height = image[0].size();
    int width = image[0][0].size();

    vector<unsigned char> data(height * width * channels);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            for (int c = 0; c < channels; ++c)
            {
                data[(y * width + x) * channels + c] = image[c][y][x];
            }
        }
    }
    if (!stbi_write_png(filename.c_str(), width, height, channels, data.data(), width * channels))
    {
        throw runtime_error("Failed to write image");
    }
}

int average(const single_channel_image_t &image, int point_x, int point_y, int filter_size) {
    int pad = filter_size / 2;
    int accumulator = 0;
    for (int i = point_x - pad; i < point_x + pad; i++) {
        for (int j = point_y - pad; j < point_y + pad; j++){
            accumulator += image[i][j];
        }
    }
    return accumulator / (filter_size * filter_size);
}

single_channel_image_t apply_box_blur(const single_channel_image_t &image, const int filter_size)
{
    // Get the dimensions of the input image
    int width = image[0].size();
    int height = image.size();

    // Create a new image to store the result
    single_channel_image_t result(height, vector<uint8_t>(width));

    // Calculate the padding size for the filter
    int pad = filter_size / 2;

    //Percorre a matriz 
    for (int i = pad; i < height - pad; i++) {
        for (int j = pad; j < width - pad; j++) {
            result[i][j] = average(image, i, j, filter_size);
        }
    }
    for (int i = 0; i < pad; i++){
        for(int j = 0; j < width; j++){
            result[i][j] = image[i][j];
            result[height - i - 1][j] = image[height - i - 1][j];
        }
    }
        for (int i = 0; i < height; i++){
        for(int j = 0; j < pad; j++){
            result[i][j] = image[i][j];
            result[i][width - j - 1] = image[i][width - j - 1];
        }
    }

    return result;
}

void blur_image(string &input_image_path) {
    image_t input_image = load_image(input_image_path);
    image_t output_image;
    for (int i = 0; i < NUM_CHANNELS; ++i)
    {
        output_image[i] = apply_box_blur(input_image[i], FILTER_SIZE);
    }
    string output_image_path = input_image_path.replace(input_image_path.find(INPUT_DIRECTORY), INPUT_DIRECTORY.length(), OUTPUT_DIRECTORY);
    write_image(output_image_path, output_image);
}

int set_pre_conditions() {
    if (!filesystem::exists(INPUT_DIRECTORY))
    {
        cerr << "Error, " << INPUT_DIRECTORY << " directory does not exist" << endl;
        return -1;
    }

    if (!filesystem::exists(OUTPUT_DIRECTORY))
    {
        if (!filesystem::create_directory(OUTPUT_DIRECTORY))
        {
            cerr << "Error creating" << OUTPUT_DIRECTORY << " directory" << endl;
            return -1;
        }
    }

    if (!filesystem::is_directory(OUTPUT_DIRECTORY))
    {
        cerr << "Error there is a file named " << OUTPUT_DIRECTORY << ", it should be a directory" << endl;
        return -1;
    }
    return 0;
}

void set_images_queue_by( std::filesystem::__cxx11::directory_entry file) {
    string input_image_path = file.path().string();
    unique_lock<mutex> lock(m);
    while (imagesQueue.counter == imagesQueue.BUFFER_SIZE)
    {			
        space_available.wait(lock); 
    }
    imagesQueue.add(input_image_path);
}

int producer_func()
{
    if (set_pre_conditions() < 0) return 1;
	while (true)		
	{
        auto start_time = chrono::high_resolution_clock::now();
        for (auto &file : filesystem::directory_iterator{INPUT_DIRECTORY})
        {
            set_images_queue_by(file);
        }
		data_available.notify_one();
		if (SLEEP_TIME > 0)
			this_thread::sleep_for(chrono::milliseconds(SLEEP_TIME));
		assert(imagesQueue.counter <= imagesQueue.BUFFER_SIZE);
	}
}

// Consumer
void consumer_func(const unsigned id)
{
	while (true)
	{
		unique_lock<mutex> lock(m);
		
		while (imagesQueue.counter == 0)
		{
			data_available.wait(lock);
		}

		string i = imagesQueue.get();
        blur_image(i);

		space_available.notify_one();
		if (SLEEP_TIME > 0)
			this_thread::sleep_for(chrono::milliseconds(SLEEP_TIME));

		assert(imagesQueue.counter >= 0);
    }
}

// Image type definition


int main(int argc, char *argv[])
{
    vector<thread> producers;
	vector<thread> consumers;

	for (unsigned i =0; i < NUM_PRODUCERS; ++i)
	{
		producers.push_back(thread(producer_func));
	}
	for (unsigned i =0; i < NUM_CONSUMERS; ++i)
	{
		consumers.push_back(thread(consumer_func, i));
	}
	consumers[0].join();
    return 0;
}
