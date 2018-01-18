#include "Session.h"
#include "events.h"
#include "rapidjson/pointer.h"
using namespace HighFive;
using namespace std;
using namespace uWS;
using namespace rapidjson;

Session::Session(WebSocket<SERVER>* ws, boost::uuids::uuid uuid, string folder)
	:uuid(uuid),
	 currentBand(-1),
	 file(nullptr),
	 baseFolder(folder),
	 socket(ws)
{
}

Session::~Session()
{
	delete file;
}

void Session::updateHistogram()
{
	auto numRows = imageInfo.height;
	if (!numRows)
		return;
	auto rowSize = imageInfo.width;
	if (!rowSize)
		return;

	float minVal = currentBandCache[0][0][0];
	float maxVal = currentBandCache[0][0][0];

	for (auto i = 0; i < imageInfo.height; i++)
	{
		for (auto j = 0; j < imageInfo.width; j++)
		{
			minVal = fmin(minVal, currentBandCache[0][i][j]);
			maxVal = fmax(maxVal, currentBandCache[0][i][j]);
		}
	}

	currentBandHistogram.N = max(sqrt(imageInfo.width * imageInfo.height), 2.0);
	currentBandHistogram.binWidth = (maxVal - minVal) / currentBandHistogram.N;
	currentBandHistogram.firstBinCenter = minVal + currentBandHistogram.binWidth / 2.0f;
	currentBandHistogram.bins.resize(currentBandHistogram.N);
	memset(currentBandHistogram.bins.data(), 0, sizeof(int)*currentBandHistogram.bins.size());
	for (auto i = 0; i < imageInfo.height; i++)
	{
		for (auto j = 0; j < imageInfo.width; j++)
		{
			auto v = currentBandCache[0][i][j];
			if (isnan(v))
				continue;
			int bin = min((int) ((v - minVal) / currentBandHistogram.binWidth), currentBandHistogram.N - 1);
			currentBandHistogram.bins[bin]++;
		}
	}
}


bool Session::parseRegionQuery(const Value& message, ReadRegionRequest& regionQuery)
{
	const char* intVarNames[] = {"x", "y", "w", "h", "band", "mip", "compression"};

	for (auto varName:intVarNames)
	{
		if (!message.HasMember(varName) || !message[varName].IsInt())
			return false;
	}

	regionQuery = {message["x"].GetInt(), message["y"].GetInt(), message["w"].GetInt(), message["h"].GetInt(), message["band"].GetInt(), message["mip"].GetInt(), message["compression"].GetInt()};
	if (regionQuery.x < 0 || regionQuery.y < 0 || regionQuery.band < 0 || regionQuery.band >= imageInfo.numBands || regionQuery.mip < 1 || regionQuery.w < 1 || regionQuery.h < 1)
		return false;
	return true;
}

bool Session::loadBand(int band)
{

	if (!file || !file->isValid())
	{

		log("No file loaded");
		return false;
	}
	else if (band >= imageInfo.numBands)
	{
		log(fmt::format("Invalid band for band {} in file {}", band, imageInfo.filename));
		return false;
	}

	dataSets[0].select({band, 0, 0}, {1, imageInfo.height, imageInfo.width}).read(currentBandCache);
	updateHistogram();
	currentBand = band;
	return true;
}

bool Session::loadFile(const string& filename, int defaultBand)
{
	if (filename == imageInfo.filename)
		return true;

	delete file;

	try
	{
		file = new File(filename, File::ReadOnly);
		vector<string> fileObjectList = file->listObjectNames();
		imageInfo.filename = filename;
		auto group = file->getGroup("Image");
		DataSet dataSet = group.getDataSet("Data");

		auto dims = dataSet.getSpace().getDimensions();
		if (dims.size() != 3)
		{
			log(fmt::format("Problem loading file {}: Data is not a valid 3D array.", filename));
			return false;
		}

		imageInfo.numBands = dims[0];
		imageInfo.height = dims[1];
		imageInfo.width = dims[2];
		dataSets.clear();
		dataSets.emplace_back(dataSet);

		if (group.exist("DataSwizzled"))
		{
			DataSet dataSetSwizzled = group.getDataSet("DataSwizzled");
			auto swizzledDims = dataSetSwizzled.getSpace().getDimensions();
			if (swizzledDims.size() != 3 || swizzledDims[0] != dims[2])
			{
				log(fmt::format("Invalid swizzled data set in file {}, ignoring.", filename));
			}
			else
			{
				log(fmt::format("Found valid swizzled data set in file {}.", filename));
				dataSets.emplace_back(dataSetSwizzled);
			}
		}
		else
		{
			log(fmt::format("File {} missing optional swizzled data set, using fallback calculation.\n", filename));
		}
		return loadBand(defaultBand);
	}
	catch (HighFive::Exception& err)
	{
		log(fmt::format("Problem loading file {}", filename));
		return false;
	}
}

vector<float> Session::getZProfile(int x, int y)
{
	if (!file || !file->isValid())
	{
		log("No file loaded or invalid session");
		return vector<float>();
	}
	else if (x >= imageInfo.width || y >= imageInfo.height)
	{
		log("Z profile out of range");
		return vector<float>();
	}

	try
	{
		vector<float> profile;

		if (dataSets.size() == 2)
		{
			Matrix3F zP;
			dataSets[1].select({x, y, 0}, {1, 1, imageInfo.numBands}).read(zP);
			profile.resize(imageInfo.numBands);
			memcpy(profile.data(), zP.data(), imageInfo.numBands * sizeof(float));
		}
		else
		{
			dataSets[0].select({0, y, x}, {imageInfo.numBands, 1, 1}).read(profile);
		}
		return profile;
	}
	catch (HighFive::Exception& err)
	{
		log(fmt::format("Invalid profile request in file {}", imageInfo.filename));
		return vector<float>();
	}
}

vector<float> Session::readRegion(const ReadRegionRequest& req)
{
	if (!file || !file->isValid())
	{
		log("No file loaded");
		return vector<float>();
	}

	if (currentBand != req.band)
	{
		if (!loadBand(req.band))
		{
			log(fmt::format("Select band {} is invalid!", req.band));
			return vector<float>();
		}
	}


	if (imageInfo.height < req.y + req.h || imageInfo.width < req.x + req.w)
	{
		log(fmt::format("Selected region ({}, {}) -> ({}, {} in band {} is invalid!", req.x, req.y, req.x + req.w, req.x + req.h, req.band));
		return vector<float>();
	}

	size_t numRowsRegion = req.h / req.mip;
	size_t rowLengthRegion = req.w / req.mip;
	vector<float> regionData;
	regionData.resize(numRowsRegion * rowLengthRegion);

	for (auto j = 0; j < numRowsRegion; j++)
	{
		for (auto i = 0; i < rowLengthRegion; i++)
		{
			float sumPixel = 0;
			int count = 0;
			for (auto x = 0; x < req.mip; x++)
			{
				for (auto y = 0; y < req.mip; y++)
				{
					float pixVal = currentBandCache[0][req.y + j * req.mip + y][req.x + i * req.mip + x];
					if (!isnan(pixVal))
					{
						count++;
						sumPixel += pixVal;
					}
				}
			}
			regionData[j * rowLengthRegion + i] = count ? sumPixel / count : NAN;
		}
	}
	return regionData;
}


void Session::onRegionRead(const Value& message)
{
	eventMutex.lock();
	ReadRegionRequest request;

	if (parseRegionQuery(message, request))
	{
		auto tStart = std::chrono::high_resolution_clock::now();
		bool compressed = request.compression >= 4 && request.compression < 32;
		vector<float> regionData = readRegion(request);
		if (regionData.size())
		{
			auto tEnd = std::chrono::high_resolution_clock::now();
			auto dtRegion = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
			auto numValues = regionData.size();
			auto rowLength = request.w / request.mip;
			auto numRows = request.h / request.mip;

			Document d(kObjectType);
			auto& a = d.GetAllocator();
			d.AddMember("event", "region_read", d.GetAllocator());

			Value responseMessage(kObjectType);
			responseMessage.AddMember("success", true, d.GetAllocator());
			responseMessage.AddMember("compression", request.compression, a);
			responseMessage.AddMember("x", request.x, a);
			responseMessage.AddMember("y", request.y, a);
			responseMessage.AddMember("w", rowLength, a);
			responseMessage.AddMember("h", numRows, a);
			responseMessage.AddMember("mip", request.mip, a);
			responseMessage.AddMember("band", request.band, a);
			responseMessage.AddMember("numValues", numValues, a);

			if (currentBandHistogram.bins.size())
			{
				Value hist(kObjectType);
				hist.AddMember("firstBinCenter", currentBandHistogram.firstBinCenter, a);
				hist.AddMember("binWidth", currentBandHistogram.binWidth, a);
				hist.AddMember("N", currentBandHistogram.N, a);

				Value binsValue(kArrayType);
				binsValue.Reserve(currentBandHistogram.N, a);
				for (auto& v: currentBandHistogram.bins)
					binsValue.PushBack(v, a);

				hist.AddMember("bins", binsValue, a);
				responseMessage.AddMember("hist", hist, a);
			}
			d.AddMember("message", responseMessage, a);


			tStart = std::chrono::high_resolution_clock::now();

			tEnd = std::chrono::high_resolution_clock::now();
			auto dtPayload = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();

			tStart = std::chrono::high_resolution_clock::now();
			if (compressed)
			{
				auto nanEncoding = getNanEncodings(regionData.data(), regionData.size());
				size_t compressedSize;
				unsigned char* compressionBuffer;
				compress(regionData.data(), compressionBuffer, compressedSize, rowLength, numRows, request.compression);
				//decompress(dataPayload, compressionBuffer, compressedSize, rowLength, numRows, request.compression);

				char* binaryPayload = new char[rowLength * numRows];
				int32_t numNanEncodings = nanEncoding.size();
				memcpy(binaryPayload, &numNanEncodings, sizeof(int32_t));
				memcpy(binaryPayload + sizeof(int32_t), nanEncoding.data(), sizeof(int32_t) * numNanEncodings);
				memcpy(binaryPayload + sizeof(int32_t) + sizeof(int32_t) * numNanEncodings, compressionBuffer, compressedSize);
				uint32_t payloadSize = sizeof(int32_t) + sizeof(int32_t) * numNanEncodings + compressedSize;
				tEnd = std::chrono::high_resolution_clock::now();
				eventMutex.unlock();
				sendEventBinaryPayload(socket, d, binaryPayload, payloadSize);
				delete[] compressionBuffer;
				delete[] binaryPayload;
				auto dtCompress = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();

				log(fmt::format("Compressed binary ({:.3f} MB) sent in in {} ms", compressedSize / 1e6, dtCompress));
			}
			else
			{
				eventMutex.unlock();
				sendEventBinaryPayload(socket, d, regionData.data(), numRows * rowLength * sizeof(float));
				tEnd = std::chrono::high_resolution_clock::now();
				auto dtSent = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
				log(fmt::format("Uncompressed binary ({:.3f} MB) sent in in {} ms", numRows * rowLength * sizeof(float) / 1e6, dtSent));

			}
			return;
		}
		else
		{
			log("ReadRegion request is out of bounds");
		}
	}
	log("Event is not a valid ReadRegion request!");

	Document d;
	Pointer("/event").Set(d, "region_read");
	Pointer("/message/success").Set(d, false);
	eventMutex.unlock();
	sendEvent(socket, d);
}


void Session::onFileLoad(const Value& message)
{
	eventMutex.lock();
	if (message.HasMember("filename") && message["filename"].IsString())
	{
		string filename = message["filename"].GetString();
		if (loadFile(fmt::format("{}/{}", baseFolder, filename)))
		{
			log(fmt::format("File {} loaded successfully", filename));
			Document d;
			Pointer("/message/numBands").Set(d, imageInfo.numBands);
			Pointer("/message/success").Set(d, true);
			Pointer("/event").Set(d, "fileload");
			eventMutex.unlock();
			sendEvent(socket, d);
			//profileReads();


			return;
		}
		else
		{
			log(fmt::format("Error loading file {}", filename));
		}
	}

	Document d;
	Pointer("/event").Set(d, "fileload");
	Pointer("/message/success").Set(d, false);
	eventMutex.unlock();
	sendEvent(socket, d);
}

void Session::log(const string& logMessage)
{
	fmt::print("Session {}: {}\n", boost::uuids::to_string(uuid), logMessage);
}

void Session::profileReads()
{
	// Profile Z-Profile reads
	vector<float> zProfile;

	vector<float> readTimes;
	srand(time(NULL));
	for (auto i = 0; i < 10; i++)
	{
		auto tStart = chrono::high_resolution_clock::now();
		int randX = ((float) rand()) / RAND_MAX * imageInfo.width;
		int randY = ((float) rand()) / RAND_MAX * imageInfo.height;
		zProfile = getZProfile(randX, randY);
		auto tEnd = chrono::high_resolution_clock::now();
		auto dtZProfile = std::chrono::duration_cast<chrono::milliseconds>(tEnd - tStart).count();
		readTimes.push_back(dtZProfile);
	}

	float sumX = 0;
	float sumX2 = 0;
	float minVal = readTimes[0];
	float maxVal = readTimes[0];

	for (auto& dt: readTimes)
	{
		sumX += dt;
		sumX2 += dt * dt;
		minVal = min(minVal, dt);
		maxVal = max(maxVal, dt);
	}

	float mean = sumX / readTimes.size();
	float sigma = sqrt(sumX2 / readTimes.size() - mean * mean);
	log(fmt::format("Z Profile reads: N={}; mean={} ms; sigma={} ms; Range: {} -> {} ms\n", readTimes.size(), mean, sigma, minVal, maxVal));


	// Profile Band reads
	vector<float> readTimesBand;
	for (auto i = 0; i < 10; i++)
	{
		auto tStart = std::chrono::high_resolution_clock::now();
		int randZ = ((float) rand()) / RAND_MAX * imageInfo.numBands;
		loadBand(randZ);
		auto tEnd = std::chrono::high_resolution_clock::now();
		auto dtBand = std::chrono::duration_cast<chrono::milliseconds>(tEnd - tStart).count();
		readTimesBand.push_back(dtBand);
	}

	sumX = 0;
	sumX2 = 0;
	minVal = readTimesBand[0];
	maxVal = readTimesBand[0];

	for (auto& dt: readTimesBand)
	{
		sumX += dt;
		sumX2 += dt * dt;
		minVal = min(minVal, dt);
		maxVal = max(maxVal, dt);
	}

	mean = sumX / readTimesBand.size();
	sigma = sqrt(sumX2 / readTimesBand.size() - mean * mean);
	log(fmt::format("Band reads: N={}; mean={} ms; sigma={} ms; Range: {} -> {} ms\n", readTimesBand.size(), mean, sigma, minVal, maxVal));
}


