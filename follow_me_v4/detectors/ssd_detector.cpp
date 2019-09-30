#include "ssd_detector.h"
#include "ssd/sampleUffSSD.h"

std::vector<std::pair<int64_t, nvinfer1::DataType>> SSDDetector::calculateBindingBufferSizes(const ICudaEngine& engine, int nbBindings, int batchSize)
{
    std::vector<std::pair<int64_t, nvinfer1::DataType>> sizes;
    for (int i = 0; i < nbBindings; ++i)
    {
        Dims dims = engine.getBindingDimensions(i);
        nvinfer1::DataType dtype = engine.getBindingDataType(i);

        int64_t eltCount = samplesCommon::volume(dims) * batchSize;
        sizes.push_back(std::make_pair(eltCount, dtype));
    }

    return sizes;
}

SSDDetector::~SSDDetector()
{
	destroy();
}

void SSDDetector::allocate_buffers(IExecutionContext *context)
{
	// for now set batch size to 1
	int batchSize = 1;

	// get engine
    const ICudaEngine& engine = context->getEngine();

    // Input and output buffer pointers that we pass to the engine - the engine requires exactly IEngine::getNbBindings(),
    // of these, but in this case we know that there is exactly 1 input and 2 output.
    int num_of_bindings = engine.getNbBindings();
    buffers.reserve(num_of_bindings);
    std::vector<std::pair<int64_t, nvinfer1::DataType>> buffersSizes = calculateBindingBufferSizes(engine, num_of_bindings, batchSize);

    for (int i = 0; i < num_of_bindings; ++i)
    {
        auto bufferSizesOutput = buffersSizes[i];
        buffers[i] = samplesCommon::safeCudaMalloc(bufferSizesOutput.first * samplesCommon::getElementSize(bufferSizesOutput.second));
    }
}

ICudaEngine* SSDDetector::load_engine()
{
    // read the model in memory
    std::stringstream trtModelStream;
    trtModelStream.seekg(0, trtModelStream.beg);
    std::ifstream cache("../detectors/ssd/ssd_float32.engine");
    assert(cache.good());
    trtModelStream << cache.rdbuf();
    cache.close();

    // calculate model size
    trtModelStream.seekg(0, std::ios::end);
    const int modelSize = trtModelStream.tellg();
    trtModelStream.seekg(0, std::ios::beg);
    void* modelMem = malloc(modelSize);
    trtModelStream.read((char*) modelMem, modelSize);

    // deserialize the TensorRT engine
    IRuntime* runtime = createInferRuntime(gLogger);
    ICudaEngine* engine = runtime->deserializeCudaEngine(modelMem, modelSize, nullptr);
    free(modelMem);
    runtime->destroy();
    return engine;
}

// initialization
void SSDDetector::init()
{
	init(0.4);
}

void SSDDetector::init(double confidence_threshold)
{
	// set confidence threshold
	this->confidence_threshold = confidence_threshold;

    // init nvidia inference plugins library
	std::cout << "SSD: Initializing NvInferPlugins" << std::endl;
    initLibNvInferPlugins(&gLogger, "");

    // load the model and create the TensorRT engine
    std::cout << "SSD: loading TensorRT engine" << std::endl;
	engine = load_engine();

	std::cout << "SSD: creating execution context" << std::endl;
	context = engine->createExecutionContext();

	// create the CUDA stream
	std::cout << "SSD: creating CUDA stream" << std::endl;
    CHECK(cudaStreamCreate(&stream));

    // allocate the buffers
    std::cout << "SSD: allocating buffers" << std::endl;
    allocate_buffers(context);
}

// detect objects and return their bounding boxes and class names
void SSDDetector::detect(Mat &image, std::vector<Rect2d> &out_bboxes, std::vector<std::string> &class_names)
{
    vector<float> detectionOut(100000);
    vector<int> keepCount(1);

    // letterbox image to the input size

    // subtract each channel with mean pixel values
    Mat image_zero_mean;
    subtract(image, Scalar(123.68, 116.779, 103.939), image_zero_mean);

    // split image to channels
    Mat image_split[3];
    split(image_zero_mean, image_split);

    std::cout << image.rows << " " << image.cols << std::endl;

	do_inference(*context, image_split, &detectionOut[0], &keepCount[0], 1);
	decode_outputs(&detectionOut[0], &keepCount[0], out_bboxes, class_names);
}

// do the inference
void SSDDetector::do_inference(IExecutionContext& context, Mat* inputChannels, float* detectionOut, int* keepCount, int batchSize)
{
    // In order to bind the buffers, we need to know the names of the input and output tensors.
    // Note that indices are guaranteed to be less than IEngine::getNbBindings().
    int inputIndex = engine->getBindingIndex(INPUT_BLOB_NAME);
    int outputIndex0 = engine->getBindingIndex(OUTPUT_BLOB_NAME0);
    int outputIndex1 = outputIndex0 + 1;

    cudaStream_t stream;
    CHECK(cudaStreamCreate(&stream));

    // DMA the input to the GPU, execute the batch asynchronously, and DMA it back:
    int channelSize = batchSize * INPUT_H * INPUT_W * sizeof(float);
    CHECK(cudaMemcpyAsync(buffers[inputIndex], inputChannels[0].data, channelSize, cudaMemcpyHostToDevice, stream));
    CHECK(cudaMemcpyAsync((int8_t*)buffers[inputIndex] + channelSize, inputChannels[1].data, channelSize, cudaMemcpyHostToDevice, stream));
    CHECK(cudaMemcpyAsync((int8_t*)buffers[inputIndex] + 2 * channelSize, inputChannels[2].data, channelSize, cudaMemcpyHostToDevice, stream));

    context.execute(batchSize, &buffers[0]);

    // copy results to host buffers
    CHECK(cudaMemcpyAsync(detectionOut, buffers[outputIndex0], batchSize * detectionOutputParam.keepTopK * 7 * sizeof(float), cudaMemcpyDeviceToHost, stream));
    CHECK(cudaMemcpyAsync(keepCount, buffers[outputIndex1], batchSize * sizeof(int), cudaMemcpyDeviceToHost, stream));

    // wait until all operations end
    cudaStreamSynchronize(stream);
}

// decode outputs (first two arguments) to bounding boxes and classes (last two arguments)
void SSDDetector::decode_outputs(float* detectionOut, int* keepCount, std::vector<Rect2d> &out_bboxes, std::vector<std::string> &class_names)
{
	// iterate over results. Each result is a 7-tuple of (image ID, image class, confidence, xmin, ymin, xmax, ymax)
	int keep_count = keepCount[0];
	double max_confidence = -1;
	for (int i = 0; i < keep_count; i++)
	{
		int id = detectionOut[i * NUM_OF_FIELDS_PER_DETECTION + 0];
		int image_class = detectionOut[i * NUM_OF_FIELDS_PER_DETECTION + 1];
		double confidence = detectionOut[i * NUM_OF_FIELDS_PER_DETECTION + 2];
		double xmin = detectionOut[i * NUM_OF_FIELDS_PER_DETECTION + 3] * INPUT_WIDTH;
		double ymin = detectionOut[i * NUM_OF_FIELDS_PER_DETECTION + 4] * INPUT_HEIGHT;
		double xmax = detectionOut[i * NUM_OF_FIELDS_PER_DETECTION + 5] * INPUT_WIDTH;
		double ymax = detectionOut[i * NUM_OF_FIELDS_PER_DETECTION + 6] * INPUT_HEIGHT;

		if (confidence > max_confidence) max_confidence = confidence;

		// add result if confidence passes the threshold
		if (confidence >= confidence_threshold)
		{
			out_bboxes.push_back(Rect2d(xmin, ymin, xmax - xmin, ymax - ymin));
			class_names.push_back("aaa");
		}
	}

	std::cout << max_confidence << std::endl;
//    std::cout << "detection out: ";
//    for (int i = 0; i < (NUM_OF_FIELDS_PER_DETECTION * keepCount[0]); i++) {
//    	std::cout << detectionOut[i] << " ";
//    }
//
//    std::cout << std::endl;
}

void SSDDetector::destroy()
{
    // release the stream
    cudaStreamDestroy(stream);

    // release the host and device buffers
    for (int i = 0; i < buffers.size(); i++)
    {
    	CHECK(cudaFree(buffers[i]));
    }
}
