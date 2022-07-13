/*
 * Copyright (c) 2019-2021, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Edited by Marcos Luciano
 * https://www.github.com/marcoslucianops
 */

#include "yolo.h"
#include "yoloPlugins.h"
#include <stdlib.h>

#ifdef OPENCV
#include "calibrator.h"
#endif

Yolo::Yolo(const NetworkInfo& networkInfo)
    : m_InputBlobName(networkInfo.inputBlobName),
      m_NetworkType(networkInfo.networkType),
      m_ConfigFilePath(networkInfo.configFilePath),
      m_WtsFilePath(networkInfo.wtsFilePath),
      m_Int8CalibPath(networkInfo.int8CalibPath),
      m_DeviceType(networkInfo.deviceType),
      m_NumDetectedClasses(networkInfo.numDetectedClasses),
      m_ClusterMode(networkInfo.clusterMode),
      m_NetworkMode(networkInfo.networkMode),
      m_InputH(0),
      m_InputW(0),
      m_InputC(0),
      m_InputSize(0),
      m_NumClasses(0),
      m_LetterBox(0),
      m_NewCoords(0),
      m_YoloCount(0),
      m_IouThreshold(0),
      m_ScoreThreshold(0),
      m_TopK(0)
{}

Yolo::~Yolo()
{
    destroyNetworkUtils();
}

nvinfer1::ICudaEngine *Yolo::createEngine (nvinfer1::IBuilder* builder, nvinfer1::IBuilderConfig* config)
{
    assert (builder);

    m_ConfigBlocks = parseConfigFile(m_ConfigFilePath);
    parseConfigBlocks();

    std::string configNMS = getAbsPath(m_WtsFilePath) + "/config_nms.txt";
    if (!fileExists(configNMS))
    {
        std::cerr << "YOLO config_nms.txt file is not specified\n" << std::endl;
        assert(0);
    }
    m_ConfigNMSBlocks = parseConfigFile(configNMS);
    parseConfigNMSBlocks();

    nvinfer1::INetworkDefinition *network = builder->createNetworkV2(0);
    if (parseModel(*network) != NVDSINFER_SUCCESS)
    {
        delete network;
        return nullptr;
    }

    std::cout << "Building the TensorRT Engine\n" << std::endl;

    if (m_NumClasses != m_NumDetectedClasses)
    {
        std::cout << "NOTE: Number of classes mismatch, make sure to set num-detected-classes=" << m_NumClasses
                  << " in config_infer file\n" << std::endl;
    }
    if (m_LetterBox == 1)
    {
        std::cout << "NOTE: letter_box is set in cfg file, make sure to set maintain-aspect-ratio=1 in config_infer file"
                  << " to get better accuracy\n" << std::endl;
    }
    if (m_ClusterMode != 4)
    {
        std::cout << "NOTE: Wrong cluster-mode is set, make sure to set cluster-mode=4 in config_infer file\n"
                  << std::endl;
    }

    if (m_NetworkMode == "INT8" && !fileExists(m_Int8CalibPath))
    {
        assert(builder->platformHasFastInt8());
#ifdef OPENCV
        std::string calib_image_list;
        int calib_batch_size;
        if (getenv("INT8_CALIB_IMG_PATH"))
            calib_image_list = getenv("INT8_CALIB_IMG_PATH");
        else
        {
            std::cerr << "INT8_CALIB_IMG_PATH not set" << std::endl;
            std::abort();
        }
        if (getenv("INT8_CALIB_BATCH_SIZE"))
            calib_batch_size = std::stoi(getenv("INT8_CALIB_BATCH_SIZE"));
        else
        {
            std::cerr << "INT8_CALIB_BATCH_SIZE not set" << std::endl;
            std::abort();
        }
        nvinfer1::Int8EntropyCalibrator2 *calibrator = new nvinfer1::Int8EntropyCalibrator2(
            calib_batch_size, m_InputC, m_InputH, m_InputW, m_LetterBox, calib_image_list, m_Int8CalibPath);
        config->setFlag(nvinfer1::BuilderFlag::kINT8);
        config->setInt8Calibrator(calibrator);
#else
        std::cerr << "OpenCV is required to run INT8 calibrator\n" << std::endl;
        assert(0);
#endif
    }

    nvinfer1::ICudaEngine *engine = builder->buildEngineWithConfig(*network, *config);
    if (engine)
        std::cout << "Building complete\n" << std::endl;
    else
        std::cerr << "Building engine failed\n" << std::endl;

    delete network;
    return engine;
}

NvDsInferStatus Yolo::parseModel(nvinfer1::INetworkDefinition& network) {
    destroyNetworkUtils();

    std::vector<float> weights = loadWeights(m_WtsFilePath, m_NetworkType);
    std::cout << "Building YOLO network\n" << std::endl;
    NvDsInferStatus status = buildYoloNetwork(weights, network);

    if (status == NVDSINFER_SUCCESS)
        std::cout << "Building YOLO network complete" << std::endl;
    else
        std::cerr << "Building YOLO network failed" << std::endl;

    return status;
}

NvDsInferStatus Yolo::buildYoloNetwork(std::vector<float>& weights, nvinfer1::INetworkDefinition& network)
{
    int weightPtr = 0;
    int channels = m_InputC;

    std::string weightsType;
    if (m_WtsFilePath.find(".weights") != std::string::npos)
        weightsType = "weights";
    else
        weightsType = "wts";

    float eps = 1.0e-5;
    if (m_NetworkType.find("yolov5") != std::string::npos)
        eps = 1.0e-3;
    else if (m_NetworkType.find("yolor") != std::string::npos)
        eps = 1.0e-4;

    nvinfer1::ITensor* data =
        network.addInput(m_InputBlobName.c_str(), nvinfer1::DataType::kFLOAT,
            nvinfer1::Dims3{static_cast<int>(m_InputC),
                static_cast<int>(m_InputH), static_cast<int>(m_InputW)});
    assert(data != nullptr && data->getDimensions().nbDims > 0);

    nvinfer1::ITensor* previous = data;
    std::vector<nvinfer1::ITensor*> tensorOutputs;
    std::vector<nvinfer1::ITensor*> yoloInputs;
    uint inputYoloCount = 0;

    int modelType = -1;

    for (uint i = 0; i < m_ConfigBlocks.size(); ++i)
    {
        assert(getNumChannels(previous) == channels);
        std::string layerIndex = "(" + std::to_string(tensorOutputs.size()) + ")";

        if (m_ConfigBlocks.at(i).at("type") == "net")
            printLayerInfo("", "layer", "     input", "     output", "weightPtr");
        
        else if (m_ConfigBlocks.at(i).at("type") == "convolutional")
        {
            std::string inputVol = dimsToString(previous->getDimensions());
            nvinfer1::ILayer* out = convolutionalLayer(
                i, m_ConfigBlocks.at(i), weights, m_TrtWeights, weightPtr, weightsType, channels, eps, previous, &network);
            previous = out->getOutput(0);
            assert(previous != nullptr);
            channels = getNumChannels(previous);
            std::string outputVol = dimsToString(previous->getDimensions());
            tensorOutputs.push_back(previous);
            std::string layerType = "conv_" + m_ConfigBlocks.at(i).at("activation");
            printLayerInfo(layerIndex, layerType, inputVol, outputVol, std::to_string(weightPtr));
        }

        else if (m_ConfigBlocks.at(i).at("type") == "implicit_add" || m_ConfigBlocks.at(i).at("type") == "implicit_mul")
        {
            std::string type;
            if (m_ConfigBlocks.at(i).at("type") == "implicit_add")
                type = "add";
            else if (m_ConfigBlocks.at(i).at("type") == "implicit_mul")
                type = "mul";
            assert(m_ConfigBlocks.at(i).find("filters") != m_ConfigBlocks.at(i).end());
            int filters = std::stoi(m_ConfigBlocks.at(i).at("filters"));
            nvinfer1::ILayer* out = implicitLayer(filters, weights, m_TrtWeights, weightPtr, &network);
            previous = out->getOutput(0);
            assert(previous != nullptr);
            channels = getNumChannels(previous);
            std::string outputVol = dimsToString(previous->getDimensions());
            tensorOutputs.push_back(previous);
            std::string layerType = "implicit_" + type;
            printLayerInfo(layerIndex, layerType, "        -", outputVol, std::to_string(weightPtr));
        }

        else if (m_ConfigBlocks.at(i).at("type") == "shift_channels" || m_ConfigBlocks.at(i).at("type") == "control_channels")
        {
            std::string type;
            if (m_ConfigBlocks.at(i).at("type") == "shift_channels")
                type = "shift";
            else if (m_ConfigBlocks.at(i).at("type") == "control_channels")
                type = "control";
            assert(m_ConfigBlocks.at(i).find("from") != m_ConfigBlocks.at(i).end());
            int from = stoi(m_ConfigBlocks.at(i).at("from"));
            if (from > 0)
                from = from - i + 1;
            assert((i - 2 >= 0) && (i - 2 < tensorOutputs.size()));
            assert((i + from - 1 >= 0) && (i + from - 1 < tensorOutputs.size()));
            assert(i + from - 1 < i - 2);
            nvinfer1::ILayer* out = channelsLayer(type, previous, tensorOutputs[i + from - 1], &network);
            previous = out->getOutput(0);
            assert(previous != nullptr);
            std::string outputVol = dimsToString(previous->getDimensions());
            tensorOutputs.push_back(previous);
            std::string layerType = type + "_channels" + ": " + std::to_string(i + from - 1);
            printLayerInfo(layerIndex, layerType, "        -", outputVol, "    -");
        }

        else if (m_ConfigBlocks.at(i).at("type") == "dropout")
        {
            // Skip dropout layer
            assert(previous != nullptr);
            tensorOutputs.push_back(previous);
            printLayerInfo(layerIndex, "dropout", "        -", "        -", "    -");
        }

        else if (m_ConfigBlocks.at(i).at("type") == "shortcut")
        {
            assert(m_ConfigBlocks.at(i).find("activation") != m_ConfigBlocks.at(i).end());
            assert(m_ConfigBlocks.at(i).find("from") != m_ConfigBlocks.at(i).end());
            std::string activation = m_ConfigBlocks.at(i).at("activation");
            int from = stoi(m_ConfigBlocks.at(i).at("from"));
            if (from > 0)
                from = from - i + 1;
            assert((i - 2 >= 0) && (i - 2 < tensorOutputs.size()));
            assert((i + from - 1 >= 0) && (i + from - 1 < tensorOutputs.size()));
            assert(i + from - 1 < i - 2);
            std::string inputVol = dimsToString(previous->getDimensions());
            std::string shortcutVol = dimsToString(tensorOutputs[i + from - 1]->getDimensions());
            nvinfer1::ILayer* out = shortcutLayer(i, activation, inputVol, shortcutVol, previous, tensorOutputs[i + from - 1], &network);
            previous = out->getOutput(0);
            assert(previous != nullptr);
            std::string outputVol = dimsToString(previous->getDimensions());
            tensorOutputs.push_back(previous);
            std::string layerType = "shortcut_" + m_ConfigBlocks.at(i).at("activation") + ": " + std::to_string(i + from - 1);
            printLayerInfo(layerIndex, layerType, "        -", outputVol, "    -");
            if (inputVol != shortcutVol) {
                std::cout << inputVol << " +" << shortcutVol << std::endl;
            }
        }

        else if (m_ConfigBlocks.at(i).at("type") == "route")
        {
            assert(m_ConfigBlocks.at(i).find("layers") != m_ConfigBlocks.at(i).end());
            nvinfer1::ILayer* out = routeLayer(i, m_ConfigBlocks.at(i), tensorOutputs, &network);
            previous = out->getOutput(0);
            assert(previous != nullptr);
            channels = getNumChannels(previous);
            std::string outputVol = dimsToString(previous->getDimensions());
            tensorOutputs.push_back(previous);
            printLayerInfo(layerIndex, "route", "        -", outputVol, std::to_string(weightPtr));
        }

        else if (m_ConfigBlocks.at(i).at("type") == "upsample")
        {
            std::string inputVol = dimsToString(previous->getDimensions());
            nvinfer1::ILayer* out = upsampleLayer(i - 1, m_ConfigBlocks[i], previous, &network);
            previous = out->getOutput(0);
            assert(previous != nullptr);
            std::string outputVol = dimsToString(previous->getDimensions());
            tensorOutputs.push_back(previous);
            printLayerInfo(layerIndex, "upsample", inputVol, outputVol, "    -");
        }

        else if (m_ConfigBlocks.at(i).at("type") == "maxpool")
        {
            std::string inputVol = dimsToString(previous->getDimensions());
            nvinfer1::ILayer* out = maxpoolLayer(i, m_ConfigBlocks.at(i), previous, &network);
            previous = out->getOutput(0);
            assert(previous != nullptr);
            std::string outputVol = dimsToString(previous->getDimensions());
            tensorOutputs.push_back(previous);
            printLayerInfo(layerIndex, "maxpool", inputVol, outputVol, std::to_string(weightPtr));
        }

        else if (m_ConfigBlocks.at(i).at("type") == "reorg")
        {
            if (m_NetworkType.find("yolov5") != std::string::npos || m_NetworkType.find("yolor") != std::string::npos)
            {
                std::string inputVol = dimsToString(previous->getDimensions());
                nvinfer1::ILayer* out = reorgV5Layer(i, previous, &network);
                previous = out->getOutput(0);
                assert(previous != nullptr);
                channels = getNumChannels(previous);
                std::string outputVol = dimsToString(previous->getDimensions());
                tensorOutputs.push_back(previous);
                std::string layerType = "reorgV5";
                printLayerInfo(layerIndex, layerType, inputVol, outputVol, std::to_string(weightPtr));
            }
            else 
            {
                std::string inputVol = dimsToString(previous->getDimensions());
                nvinfer1::IPluginV2* reorgPlugin = createReorgPlugin(2);
                assert(reorgPlugin != nullptr);
                nvinfer1::IPluginV2Layer* reorg =
                    network.addPluginV2(&previous, 1, *reorgPlugin);
                assert(reorg != nullptr);
                std::string layerName = "reorg_" + std::to_string(i);
                reorg->setName(layerName.c_str());
                previous = reorg->getOutput(0);
                assert(previous != nullptr);
                std::string outputVol = dimsToString(previous->getDimensions());
                channels = getNumChannels(previous);
                tensorOutputs.push_back(reorg->getOutput(0));
                printLayerInfo(layerIndex, "reorg", inputVol, outputVol, std::to_string(weightPtr));
            }
        }

        else if (m_ConfigBlocks.at(i).at("type") == "yolo" || m_ConfigBlocks.at(i).at("type") == "region")
        {
            if (m_ConfigBlocks.at(i).at("type") == "yolo")
            {
                if (m_NetworkType.find("yolor") != std::string::npos)
                    modelType = 2;
                else
                    modelType = 1;
            }
            else
                modelType = 0;

            std::string layerName = modelType != 0 ? "yolo_" + std::to_string(i) : "region_" + std::to_string(i);
            nvinfer1::Dims prevTensorDims = previous->getDimensions();
            TensorInfo& curYoloTensor = m_YoloTensors.at(inputYoloCount);
            curYoloTensor.blobName = layerName;
            curYoloTensor.gridSizeX = prevTensorDims.d[2];
            curYoloTensor.gridSizeY = prevTensorDims.d[1];

            std::string inputVol = dimsToString(previous->getDimensions());
            channels = getNumChannels(previous);
            tensorOutputs.push_back(previous);
            yoloInputs.push_back(previous);
            ++inputYoloCount;
            printLayerInfo(layerIndex, modelType != 0 ? "yolo" : "region", inputVol, "        -", "    -");
        }

        else
        {
            std::cout << "\nUnsupported layer type --> \"" << m_ConfigBlocks.at(i).at("type") << "\"" << std::endl;
            assert(0);
        }
    }

    if ((int)weights.size() != weightPtr)
    {
        std::cout << "\nNumber of unused weights left: " << weights.size() - weightPtr << std::endl;
        assert(0);
    }

    if (m_YoloCount == inputYoloCount)
    {
        assert((modelType != -1) && "\nCould not determine model type"); 

        nvinfer1::ITensor* yoloInputTensors[inputYoloCount];
        uint64_t outputSize = 0;
        for (uint j = 0; j < inputYoloCount; ++j)
        {
            yoloInputTensors[j] = yoloInputs[j];
            TensorInfo& curYoloTensor = m_YoloTensors.at(j);
            outputSize += curYoloTensor.gridSizeX * curYoloTensor.gridSizeY * curYoloTensor.numBBoxes;
        }

        if (m_TopK > outputSize) {
            std::cout << "\ntopk > Number of outputs\nPlease change the topk to " << outputSize
                      << " or less in config_nms.txt file\n" << std::endl;
            assert(0);
        }

        std::string layerName = "yolo";
        nvinfer1::IPluginV2* yoloPlugin = new YoloLayer(
            m_InputW, m_InputH, m_NumClasses, m_NewCoords, m_YoloTensors, outputSize, modelType, m_TopK,
            m_ScoreThreshold);
        assert(yoloPlugin != nullptr);
        nvinfer1::IPluginV2Layer* yolo = network.addPluginV2(yoloInputTensors, inputYoloCount, *yoloPlugin);
        assert(yolo != nullptr);
        yolo->setName(layerName.c_str());
        previous = yolo->getOutput(0);
        assert(previous != nullptr);
        previous->setName(layerName.c_str());
        tensorOutputs.push_back(yolo->getOutput(0));

        nvinfer1::ITensor* yoloTensors[] = {yolo->getOutput(0), yolo->getOutput(1)};
        std::string outputVol = dimsToString(previous->getDimensions());

        nvinfer1::plugin::NMSParameters nmsParams;
        nmsParams.shareLocation = true;
        nmsParams.backgroundLabelId = -1;
        nmsParams.numClasses = m_NumClasses;
        nmsParams.topK = m_TopK;
        nmsParams.keepTopK = m_TopK;
        nmsParams.scoreThreshold = m_ScoreThreshold;
        nmsParams.iouThreshold = m_IouThreshold;
        nmsParams.isNormalized = false;

        layerName = "batchedNMS";
        nvinfer1::IPluginV2* batchedNMS = createBatchedNMSPlugin(nmsParams);
        nvinfer1::IPluginV2Layer* nms = network.addPluginV2(yoloTensors, 2, *batchedNMS);
        nms->setName(layerName.c_str());
        nvinfer1::ITensor* num_detections = nms->getOutput(0);
        layerName = "num_detections";
        num_detections->setName(layerName.c_str());
        nvinfer1::ITensor* nmsed_boxes = nms->getOutput(1);
        layerName = "nmsed_boxes";
        nmsed_boxes->setName(layerName.c_str());
        nvinfer1::ITensor* nmsed_scores = nms->getOutput(2);
        layerName = "nmsed_scores";
        nmsed_scores->setName(layerName.c_str());
        nvinfer1::ITensor* nmsed_classes = nms->getOutput(3);
        layerName = "nmsed_classes";
        nmsed_classes->setName(layerName.c_str());
        network.markOutput(*num_detections);
        network.markOutput(*nmsed_boxes);
        network.markOutput(*nmsed_scores);
        network.markOutput(*nmsed_classes);

        printLayerInfo("", "batched_nms", "        -", outputVol, "    -");
    }
    else {
        std::cout << "\nError in yolo cfg file" << std::endl;
        assert(0);
    }

    std::cout << "\nOutput YOLO blob names: " << std::endl;
    for (auto& tensor : m_YoloTensors)
    {
        std::cout << tensor.blobName << std::endl;
    }

    int nbLayers = network.getNbLayers();
    std::cout << "\nTotal number of YOLO layers: " << nbLayers << "\n" << std::endl;

    return NVDSINFER_SUCCESS;
}

std::vector<std::map<std::string, std::string>>
Yolo::parseConfigFile (const std::string cfgFilePath)
{
    assert(fileExists(cfgFilePath));
    std::ifstream file(cfgFilePath);
    assert(file.good());
    std::string line;
    std::vector<std::map<std::string, std::string>> blocks;
    std::map<std::string, std::string> block;

    while (getline(file, line))
    {
        if (line.size() == 0) continue;
        if (line.front() == ' ') continue;
        if (line.front() == '#') continue;
        line = trim(line);
        if (line.front() == '[')
        {
            if (block.size() > 0)
            {
                blocks.push_back(block);
                block.clear();
            }
            std::string key = "type";
            std::string value = trim(line.substr(1, line.size() - 2));
            block.insert(std::pair<std::string, std::string>(key, value));
        }
        else
        {
            int cpos = line.find('=');
            std::string key = trim(line.substr(0, cpos));
            std::string value = trim(line.substr(cpos + 1));
            block.insert(std::pair<std::string, std::string>(key, value));
        }
    }
    blocks.push_back(block);
    return blocks;
}

void Yolo::parseConfigBlocks()
{
    for (auto block : m_ConfigBlocks)
    {
        if (block.at("type") == "net")
        {
            assert((block.find("height") != block.end()) && "Missing 'height' param in network cfg");
            assert((block.find("width") != block.end()) && "Missing 'width' param in network cfg");
            assert((block.find("channels") != block.end()) && "Missing 'channels' param in network cfg");

            m_InputH = std::stoul(block.at("height"));
            m_InputW = std::stoul(block.at("width"));
            m_InputC = std::stoul(block.at("channels"));
            m_InputSize = m_InputC * m_InputH * m_InputW;

            if (block.find("letter_box") != block.end())
            {
                m_LetterBox = std::stoul(block.at("letter_box"));
            }
        }
        else if ((block.at("type") == "region") || (block.at("type") == "yolo"))
        {
            assert((block.find("num") != block.end())
                   && std::string("Missing 'num' param in " + block.at("type") + " layer").c_str());
            assert((block.find("classes") != block.end())
                   && std::string("Missing 'classes' param in " + block.at("type") + " layer").c_str());
            assert((block.find("anchors") != block.end())
                   && std::string("Missing 'anchors' param in " + block.at("type") + " layer").c_str());

            ++m_YoloCount;

            m_NumClasses = std::stoul(block.at("classes"));

            if (block.find("new_coords") != block.end())
            {
                m_NewCoords = std::stoul(block.at("new_coords"));
            }

            TensorInfo outputTensor;

            std::string anchorString = block.at("anchors");
            while (!anchorString.empty())
            {
                int npos = anchorString.find_first_of(',');
                if (npos != -1)
                {
                    float anchor = std::stof(trim(anchorString.substr(0, npos)));
                    outputTensor.anchors.push_back(anchor);
                    anchorString.erase(0, npos + 1);
                }
                else
                {
                    float anchor = std::stof(trim(anchorString));
                    outputTensor.anchors.push_back(anchor);
                    break;
                }
            }

            if (block.find("mask") != block.end())
            {
                std::string maskString = block.at("mask");
                while (!maskString.empty())
                {
                    int npos = maskString.find_first_of(',');
                    if (npos != -1)
                    {
                        int mask = std::stoul(trim(maskString.substr(0, npos)));
                        outputTensor.mask.push_back(mask);
                        maskString.erase(0, npos + 1);
                    }
                    else
                    {
                        int mask = std::stoul(trim(maskString));
                        outputTensor.mask.push_back(mask);
                        break;
                    }
                }
            }

            if (block.find("scale_x_y") != block.end())
            {
                outputTensor.scaleXY = std::stof(block.at("scale_x_y"));
            }
            else
            {
                outputTensor.scaleXY = 1.0;
            }

            outputTensor.numBBoxes
                = outputTensor.mask.size() > 0 ? outputTensor.mask.size() : std::stoul(trim(block.at("num")));
            
            m_YoloTensors.push_back(outputTensor);
        }
    }
}

void Yolo::parseConfigNMSBlocks()
{
    auto block = m_ConfigNMSBlocks[0];

    assert((block.at("type") == "property") && "Missing 'property' param in nms cfg");
    assert((block.find("iou-threshold") != block.end()) && "Missing 'iou-threshold' param in nms cfg");
    assert((block.find("score-threshold") != block.end()) && "Missing 'score-threshold' param in nms cfg");
    assert((block.find("topk") != block.end()) && "Missing 'topk' param in nms cfg");

    m_IouThreshold = std::stof(block.at("iou-threshold"));
    m_ScoreThreshold = std::stof(block.at("score-threshold"));
    m_TopK = std::stoul(block.at("topk"));
}

void Yolo::destroyNetworkUtils()
{
    for (uint i = 0; i < m_TrtWeights.size(); ++i)
    {
        if (m_TrtWeights[i].count > 0)
            free(const_cast<void*>(m_TrtWeights[i].values));
    }
    m_TrtWeights.clear();
}
