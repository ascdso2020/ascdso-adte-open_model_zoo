/*
// Copyright (C) 2021-2022 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <openvino/openvino.hpp>
#include <utils/ocv_common.hpp>
#include <utils/slog.hpp>

#include "models/deblurring_model.h"

DeblurringModel::DeblurringModel(const std::string& modelFileName, const cv::Size& inputImgSize) :
    ImageModel(modelFileName) {
        netInputHeight = inputImgSize.height;
        netInputWidth = inputImgSize.width;
}

void DeblurringModel::prepareInputsOutputs(std::shared_ptr<ov::Model>& model) {
    // --------------------------- Configure input & output -------------------------------------------------
    // --------------------------- Prepare input ------------------------------------------------------
    const ov::OutputVector& inputsInfo = model->inputs();
    if (inputsInfo.size() != 1) {
        throw std::logic_error("Deblurring model wrapper supports topologies only with 1 input");
    }

    inputsNames.push_back(model->input().get_any_name());

    const ov::Shape& inputShape = model->input().get_shape();
    ov::Layout inputLayout = ov::layout::get_layout(model->input());
    if (inputLayout.empty()) {
        inputLayout = { "NCHW" };
    }

    if (inputShape.size() != 4 || inputShape[ov::layout::batch_idx(inputLayout)] != 1 || inputShape[ov::layout::channels_idx(inputLayout)] != 3) {
        throw std::logic_error("3-channel 4-dimensional model's input is expected");
    }

    ov::preprocess::PrePostProcessor ppp(model);
    ppp.input().tensor().
        set_element_type(ov::element::u8).
        set_layout("NHWC");

    ppp.input().model().set_layout(inputLayout);

    // --------------------------- Prepare output  -----------------------------------------------------
    const ov::OutputVector& outputsInfo = model->outputs();
    if (outputsInfo.size() != 1) {
        throw std::logic_error("Deblurring model wrapper supports topologies only with 1 output");
    }

    outputsNames.push_back(model->output().get_any_name());

    const ov::Shape& outputShape = model->output().get_shape();
    ov::Layout outputLayout{ "NCHW" };
    if (outputShape.size() != 4 || outputShape[ov::layout::batch_idx(outputLayout)] != 1 || outputShape[ov::layout::channels_idx(outputLayout)] != 3) {
        throw std::logic_error("3-channel 4-dimensional model's output is expected");
    }

    ppp.output().tensor().set_element_type(ov::element::f32);
    model = ppp.build();

    changeInputSize(model);
}

void DeblurringModel::changeInputSize(std::shared_ptr<ov::Model>& model) {
    auto inTensorName = model->input().get_any_name();
    ov::Shape inputShape = model->input().get_shape();
    ov::Layout layout = ov::layout::get_layout(model->input());

    auto batchId = ov::layout::batch_idx(layout);
    auto heightId = ov::layout::height_idx(layout);
    auto widthId = ov::layout::width_idx(layout);

    if (inputShape[heightId] % stride || inputShape[widthId] % stride) {
        throw std::logic_error("Model input shape must be divisible by stride");
    }

    netInputHeight = static_cast<int>((netInputHeight + stride - 1) / stride) * stride;
    netInputWidth = static_cast<int>((netInputWidth + stride - 1) / stride) * stride;

    inputShape[batchId] = 1;
    inputShape[heightId] = netInputHeight;
    inputShape[widthId] = netInputWidth;

    std::map<std::string, ov::PartialShape> shapes;
    shapes[inTensorName] = ov::PartialShape(inputShape);
    model->reshape(shapes);
}

std::shared_ptr<InternalModelData> DeblurringModel::preprocess(const InputData& inputData, ov::InferRequest& request) {
    auto& image = inputData.asRef<ImageInputData>().inputImage;
    size_t h = image.rows;
    size_t w = image.cols;
    cv::Mat resizedImage;

    if (netInputHeight - stride < h && h <= netInputHeight
        && netInputWidth - stride < w && w <= netInputWidth) {
        int bottom = netInputHeight - h;
        int right = netInputWidth - w;
        cv::copyMakeBorder(image, resizedImage, 0, bottom, 0, right,
                           cv::BORDER_CONSTANT, 0);
    } else {
        slog::warn << "\tChosen model aspect ratio doesn't match image aspect ratio" << slog::endl;
        cv::resize(image, resizedImage, cv::Size(netInputWidth, netInputHeight));
    }
    request.set_input_tensor(wrapMat2Tensor(resizedImage));

    return std::make_shared<InternalImageModelData>(image.cols, image.rows);
}

std::unique_ptr<ResultBase> DeblurringModel::postprocess(InferenceResult& infResult) {
    ImageResult* result = new ImageResult;
    *static_cast<ResultBase*>(result) = static_cast<ResultBase&>(infResult);

    const auto& inputImgSize = infResult.internalModelData->asRef<InternalImageModelData>();
    const auto outputData = infResult.getFirstOutputTensor().data<float>();

    std::vector<cv::Mat> imgPlanes;
    const ov::Shape& outputShape= infResult.getFirstOutputTensor().get_shape();
    size_t outHeight = (int)(outputShape[2]);
    size_t outWidth = (int)(outputShape[3]);
    size_t numOfPixels = outWidth * outHeight;
    imgPlanes = std::vector<cv::Mat>{
          cv::Mat(outHeight, outWidth, CV_32FC1, &(outputData[0])),
          cv::Mat(outHeight, outWidth, CV_32FC1, &(outputData[numOfPixels])),
          cv::Mat(outHeight, outWidth, CV_32FC1, &(outputData[numOfPixels * 2]))};
    cv::Mat resultImg;
    cv::merge(imgPlanes, resultImg);

    if (netInputHeight - stride < static_cast<size_t>(inputImgSize.inputImgHeight) && static_cast<size_t>(inputImgSize.inputImgHeight) <= netInputHeight
        && netInputWidth - stride < static_cast<size_t>(inputImgSize.inputImgWidth) && static_cast<size_t>(inputImgSize.inputImgWidth) <= netInputWidth) {
        result->resultImage = resultImg(cv::Rect(0, 0, inputImgSize.inputImgWidth, inputImgSize.inputImgHeight));
    } else {
        cv::resize(resultImg, result->resultImage, cv::Size(inputImgSize.inputImgWidth, inputImgSize.inputImgHeight));
    }

    result->resultImage.convertTo(result->resultImage, CV_8UC3, 255);

    return std::unique_ptr<ResultBase>(result);
}
