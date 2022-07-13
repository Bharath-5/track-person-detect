/*
 * Created by Marcos Luciano
 * https://www.github.com/marcoslucianops
 */

#include <stdint.h>

inline __device__ float sigmoidGPU(const float& x) { return 1.0f / (1.0f + __expf(-x)); }

__device__ void softmaxGPU(
    const float* input, const int bbindex, const int numGridCells, uint z_id, const uint numOutputClasses, float temp,
    float* output)
{
    int i;
    float sum = 0;
    float largest = -INFINITY;
    for (i = 0; i < numOutputClasses; ++i) {
        int val = input[bbindex + numGridCells * (z_id * (5 + numOutputClasses) + (5 + i))];
        largest = (val>largest) ? val : largest;
    }
    for (i = 0; i < numOutputClasses; ++i) {
        float e = __expf(input[bbindex + numGridCells * (z_id * (5 + numOutputClasses) + (5 + i))] / temp - largest / temp);
        sum += e;
        output[bbindex + numGridCells * (z_id * (5 + numOutputClasses) + (5 + i))] = e;
    }
    for (i = 0; i < numOutputClasses; ++i) {
        output[bbindex + numGridCells * (z_id * (5 + numOutputClasses) + (5 + i))] /= sum;
    }
}

__global__ void gpuRegionLayer(
    const float* input, float* softmax, int* d_indexes, float* d_scores, float* d_boxes, int* d_classes, int* countData,
    const float scoreThreshold, const uint netWidth, const uint netHeight, const uint gridSizeX, const uint gridSizeY,
    const uint numOutputClasses, const uint numBBoxes, const float* anchors)
{
    uint x_id = blockIdx.x * blockDim.x + threadIdx.x;
    uint y_id = blockIdx.y * blockDim.y + threadIdx.y;
    uint z_id = blockIdx.z * blockDim.z + threadIdx.z;

    if (x_id >= gridSizeX || y_id >= gridSizeY || z_id >= numBBoxes)
        return;

    const int numGridCells = gridSizeX * gridSizeY;
    const int bbindex = y_id * gridSizeX + x_id;

    const float objectness
        = sigmoidGPU(input[bbindex + numGridCells * (z_id * (5 + numOutputClasses) + 4)]);

    if (objectness < scoreThreshold)
        return;

    int count = (int)atomicAdd(&countData[0], 1);

    float x
        = (sigmoidGPU(input[bbindex + numGridCells * (z_id * (5 + numOutputClasses) + 0)])
          + x_id) * netWidth / gridSizeX;

    float y
        = (sigmoidGPU(input[bbindex + numGridCells * (z_id * (5 + numOutputClasses) + 1)])
          + y_id) * netHeight / gridSizeY;

    float w
        = __expf(input[bbindex + numGridCells * (z_id * (5 + numOutputClasses) + 2)])
          * anchors[z_id * 2] * netWidth / gridSizeX;

    float h
        = __expf(input[bbindex + numGridCells * (z_id * (5 + numOutputClasses) + 3)])
          * anchors[z_id * 2 + 1] * netHeight / gridSizeY;

    softmaxGPU(input, bbindex, numGridCells, z_id, numOutputClasses, 1.0, softmax);

    float maxProb = 0.0f;
    int maxIndex = -1;

    for (uint i = 0; i < numOutputClasses; ++i)
    {
        float prob
            = softmax[bbindex + numGridCells * (z_id * (5 + numOutputClasses) + (5 + i))];

        if (prob > maxProb)
        {
            maxProb = prob;
            maxIndex = i;
        }
    }

    d_indexes[count] = count;
    d_scores[count] = objectness * maxProb + 1.f;
    d_boxes[count * 4 + 0] = x - 0.5 * w;
    d_boxes[count * 4 + 1] = y - 0.5 * h;
    d_boxes[count * 4 + 2] = x + 0.5 * w;
    d_boxes[count * 4 + 3] = y + 0.5 * h;
    d_classes[count] = maxIndex;
}

cudaError_t cudaRegionLayer(
    const void* input, void* softmax, void* d_indexes, void* d_scores, void* d_boxes, void* d_classes, void* countData,
    const uint& batchSize, uint64_t& inputSize, uint64_t& outputSize, const float& scoreThreshold, const uint& netWidth,
    const uint& netHeight, const uint& gridSizeX, const uint& gridSizeY, const uint& numOutputClasses, const uint& numBBoxes,
    const void* anchors, cudaStream_t stream);

cudaError_t cudaRegionLayer(
    const void* input, void* softmax, void* d_indexes, void* d_scores, void* d_boxes, void* d_classes, void* countData,
    const uint& batchSize, uint64_t& inputSize, uint64_t& outputSize, const float& scoreThreshold, const uint& netWidth,
    const uint& netHeight, const uint& gridSizeX, const uint& gridSizeY, const uint& numOutputClasses, const uint& numBBoxes,
    const void* anchors, cudaStream_t stream)
{
    dim3 threads_per_block(16, 16, 4);
    dim3 number_of_blocks((gridSizeX / threads_per_block.x) + 1,
                          (gridSizeY / threads_per_block.y) + 1,
                          (numBBoxes / threads_per_block.z) + 1);

    for (unsigned int batch = 0; batch < batchSize; ++batch)
    {
        gpuRegionLayer<<<number_of_blocks, threads_per_block, 0, stream>>>(
            reinterpret_cast<const float*>(input) + (batch * inputSize),
            reinterpret_cast<float*>(softmax) + (batch * inputSize),
            reinterpret_cast<int*>(d_indexes) + (batch * outputSize),
            reinterpret_cast<float*>(d_scores) + (batch * outputSize),
            reinterpret_cast<float*>(d_boxes) + (batch * 4 * outputSize),
            reinterpret_cast<int*>(d_classes) + (batch * outputSize), reinterpret_cast<int*>(countData) + (batch),
            scoreThreshold, netWidth, netHeight, gridSizeX, gridSizeY, numOutputClasses, numBBoxes,
            reinterpret_cast<const float*>(anchors));
    }
    return cudaGetLastError();
}
