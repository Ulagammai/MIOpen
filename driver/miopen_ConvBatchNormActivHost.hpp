/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#ifndef MIO_BATCHNORMACTIVHOST_H_
#define MIO_BATCHNORMACTIVHOST_H_

#include <cmath>
#include <iomanip>
#include <miopen/miopen.h>
#include <miopen/tensor.hpp>

#define MIO_HEIRARCH_SEL 0

#if(MIO_HEIRARCH_SEL == 1)
#define MIO_BN_DIST 32
#endif

template <typename T>
int miopenBNActiveBNSpatialFwdInferHost(miopenTensorDescriptor_t inputTensor,
                                        const T* in_ptr,
                                        T* out_ptr,
                                        T* scale_ptr,
                                        T* bias_ptr,
                                        double epsilon,
                                        bool estmeanvar,
                                        T* estimatedMean,
                                        T* estimatedVariance)
{
    int nIn, cIn, hIn, wIn;
    miopenGet4dTensorDescriptorLengths(inputTensor, &nIn, &cIn, &hIn, &wIn);

    int n_batchs = nIn;
    int channels = cIn;
    int height   = hIn;
    int width    = wIn;

    unsigned int index;
    unsigned int adjIndex;
    unsigned int in_nstride = channels * height * width;
    unsigned int in_cstride = height * width;

    double elemStd = 0.;
    int ret        = 0;

    if(estmeanvar)
    {

        double variance = 0.;
        double mean     = 0.;
        double inhat    = 0.;
        for(int cidx = 0; cidx < channels; cidx++)
        { // via channel
            mean             = estimatedMean[cidx];
            variance         = estimatedVariance[cidx];
            double invertVar = 1.0 / sqrt(variance + epsilon);
            // process the batch per channel
            for(int row = 0; row < height; row++)
            { // via rows
                for(int column = 0; column < width; column++)
                { // via columns
                    adjIndex = in_cstride * cidx + width * row + column;
                    for(int bidx = 0; bidx < n_batchs; bidx++)
                    { // via mini_batch
                        index          = in_nstride * bidx + adjIndex;
                        elemStd        = in_ptr[index] - mean;
                        inhat          = elemStd * invertVar;
                        out_ptr[index] = scale_ptr[cidx] * inhat + bias_ptr[cidx];
                    } // end for (n)
                }
            }
        }
    }
    else
    {

#if(MIO_HEIRARCH_SEL == 1)
        double variance_accum_arr[MIO_BN_DIST];
        double mean_accum_arr[MIO_BN_DIST];
#endif

        double variance_accum = 0.;
        double mean_accum     = 0.;
        for(int cidx = 0; cidx < channels; cidx++)
        { // via channel
#if(MIO_HEIRARCH_SEL == 1)
            for(int i = 0; i < MIO_BN_DIST; i++)
            {
                variance_accum_arr[i] = 0.;
                mean_accum_arr[i]     = 0.;
            }
#endif

            mean_accum = 0.;
#if(MIO_HEIRARCH_SEL == 0)
            // process the batch per channel
            for(int row = 0; row < height; row++)
            { // via rows
                for(int column = 0; column < width; column++)
                { // via columns
                    adjIndex = in_cstride * cidx + width * row + column;
                    for(int bidx = 0; bidx < n_batchs; bidx++)
                    { // via mini_batch
                        index = in_nstride * bidx + adjIndex;
                        // #1 calculate the mean
                        // iterating through the stack of images in the mini_batch
                        mean_accum += in_ptr[index];
                    } // end for (n)
                }     // end for (column)
            }         // end for (row)
#else
            int imgIndex = 0;
            // process the batch per channel
            for(int im = 0; im < in_cstride; im += MIO_BN_DIST)
            {
                for(int i = 0; i < MIO_BN_DIST; i++)
                {
                    imgIndex = im + i;
                    adjIndex = in_cstride * cidx + imgIndex;
                    for(int bidx = 0; bidx < n_batchs; bidx++)
                    { // via mini_batch
                        index = in_nstride * bidx + adjIndex;
                        // #1 calculate the mean
                        // iterating through the stack of images in the mini_batch
                        mean_accum_arr[i] += in_ptr[index];
                    } // end for (n)
                }     // end for (column)
            }         // end for (row)
            for(int i = 0; i < MIO_BN_DIST; i++)
            {
                mean_accum += mean_accum_arr[i];
            }
#endif
            mean_accum /= double(in_cstride * n_batchs);

            elemStd        = 0.;
            variance_accum = 0.;
#if(MIO_HEIRARCH_SEL == 0)
            // #2 calculate the variances
            // sigma^2 = (1/batch_mean) * sum( (x_i - batch_mean)^2 )
            for(int row = 0; row < height; row++)
            { // via rows
                for(int column = 0; column < width; column++)
                { // via columns
                    adjIndex = in_cstride * cidx + width * row + column;
                    for(int bidx = 0; bidx < n_batchs; bidx++)
                    { // via mini_batch
                        // per (x-dims) channel load a block of data into LDS
                        index = in_nstride * bidx + adjIndex;

                        // using out buffer as scratchpad
                        out_ptr[index] = elemStd = (in_ptr[index] - mean_accum); // (x_i - mean)
                        variance_accum += (elemStd * elemStd); // sum{ (x_i - mean)^2 }
                    }                                          // end for(n)
                }                                              // end for (column)
            }                                                  // end for (row)
#else
            for(int im = 0; im < in_cstride; im += MIO_BN_DIST)
            {
                for(int i = 0; i < MIO_BN_DIST; i++)
                {
                    imgIndex = im + i;
                    adjIndex = in_cstride * cidx + imgIndex;
                    for(int bidx = 0; bidx < n_batchs; bidx++)
                    { // via mini_batch
                        // per (x-dims) channel load a block of data into LDS
                        index = in_nstride * bidx + adjIndex;

                        // using out buffer as scratchpad
                        out_ptr[index] = elemStd = (in_ptr[index] - mean_accum); // (x_i - mean)
                        variance_accum_arr[i] += (elemStd * elemStd); // sum{ (x_i - mean)^2 }
                    }                                                 // end for(n)
                }                                                     // end for
            }                                                         // end for
            for(int i = 0; i < MIO_BN_DIST; i++)
            {
                variance_accum += variance_accum_arr[i];
            }
#endif
            variance_accum /= double(in_cstride * n_batchs); // (1/N)*sum{ (x_i - mean)^2 }

            // #3 add epsilon for numeric stability, sqr_root, and invert
            double invertVar = 1.0 / sqrt(variance_accum + epsilon);

            // #4 apply the normalization
            // x_hat = (x_i - mean) / sqrt(variance_accum - epsilon)
            for(int row = 0; row < height; row++)
            { // via rows
                for(int column = 0; column < width; column++)
                { // via columns
                    adjIndex = in_cstride * cidx + width * row + column;
                    for(int bidx = 0; bidx < n_batchs; bidx++)
                    { // via mini_batch
                        index = in_nstride * bidx + adjIndex;
                        // per (x-dims) channel load a block of data into LDS
                        // elemStd = in_ptr[index] - mean_accum;// (x_i - mean)
                        elemStd      = out_ptr[index]; // using saved values from output tensor
                        double inhat = elemStd * invertVar;
                        // #5 Gamma and Beta adjust
                        // y_i = gamma*x_hat + beta
                        out_ptr[index] = scale_ptr[cidx] * inhat + bias_ptr[cidx];
                    } // end for(n_batchs)
                }     // for (column)
            }         // for (row)
        }             // for (channel)
    }                 // end if
    return (ret);
}

template <typename T>
int miopenBNActiveBNPerActivFwdInferHost(miopenTensorDescriptor_t inputTensor,
                                         const T* in_ptr,
                                         T* out_ptr,
                                         T* scale_ptr,
                                         T* bias_ptr,
                                         double epsilon,
                                         bool estmeanvar,
                                         T* estimatedMean,
                                         T* estimatedVariance)
{ // use running mean and variance

    int nIn, cIn, hIn, wIn;
    miopenGet4dTensorDescriptorLengths(inputTensor, &nIn, &cIn, &hIn, &wIn);

    int n_batchs = nIn;
    int channels = cIn;
    int height   = hIn;
    int width    = wIn;

    // C*H*W is also stored as in_nstride, H*W is in_cstride, W is in_hstride.
    unsigned int index;
    unsigned int adjIndex;
    unsigned int in_nstride = channels * height * width;
    unsigned int in_cstride = height * width;

    double elemStd = 0.;

    int ret = 0;
    if(estmeanvar)
    {

        printf("Running estimated mean / var inference on CPU.\n");
        double mean     = 0.;
        double variance = 0.;
        for(int cidx = 0; cidx < channels; cidx++)
        { // via channel
            // process the batch per channel
            for(int row = 0; row < height; row++)
            { // via rows
                for(int column = 0; column < width; column++)
                { // via columns
                    adjIndex          = in_cstride * cidx + width * row + column;
                    mean              = estimatedMean[adjIndex];
                    variance          = estimatedVariance[adjIndex];
                    double elemInvVar = 1.0 / double(sqrt(variance + epsilon));
                    for(int bidx = 0; bidx < n_batchs; bidx++)
                    { // via mini_batch
                        index = in_nstride * bidx + adjIndex;
                        // per (x-dims) channel load a block of data into LDS
                        elemStd      = in_ptr[index] - mean; // (x_i - mean)
                        double inhat = elemStd * elemInvVar;
                        // #5 Gamma and Beta adjust
                        // y_i = gamma*x_hat + beta
                        out_ptr[index] = scale_ptr[adjIndex] * inhat + bias_ptr[adjIndex];
                    } // end for(n_batchs)
                }     // for (column)
            }
        }
    }
    else
    {

        double mean_accum     = 0.;
        double variance_accum = 0.;
        for(int cidx = 0; cidx < channels; cidx++)
        { // via channel
            // process the batch per channel
            for(int row = 0; row < height; row++)
            { // via rows
                for(int column = 0; column < width; column++)
                { // via columns
                    mean_accum = 0.;
                    adjIndex   = in_cstride * cidx + width * row + column;
                    for(int bidx = 0; bidx < n_batchs; bidx++)
                    { // via mini_batch
                        index = in_nstride * bidx + adjIndex;
                        // #1 calculate the mean
                        // iterating through the stack of images in the mini_batch
                        mean_accum += in_ptr[index];
                    }
                    mean_accum /= double(n_batchs);

                    elemStd        = 0.;
                    variance_accum = 0.;
                    // #2 calculate the variances
                    // sigma^2 = (1/batch_mean) * sum( (x_i - batch_mean)^2 )
                    for(int bidx = 0; bidx < n_batchs; bidx++)
                    { // via mini_batch
                        // per (x-dims) channel load a block of data into LDS
                        index   = in_nstride * bidx + adjIndex;
                        elemStd = in_ptr[index] - mean_accum; // (x_i - mean)
                        variance_accum += elemStd * elemStd;  // sum{ (x_i - mean)^2 }
                    }                                         // end for(n)
                    variance_accum /= double(n_batchs);       // (1/N)*sum{ (x_i - mean)^2 }

                    // #3 add epsilon for numeric stability, sqr_root, and invert
                    double elemInvVar = 1.0 / double(sqrt(variance_accum + epsilon));

                    // #4 apply the normalization
                    // x_hat = (x_i - mean) / sqrt(variance_accum - epsilon)
                    for(int bidx = 0; bidx < n_batchs; bidx++)
                    { // via mini_batch
                        index = in_nstride * bidx + adjIndex;
                        // per (x-dims) channel load a block of data into LDS
                        elemStd      = in_ptr[index] - mean_accum; // (x_i - mean)
                        double inhat = elemStd * elemInvVar;
                        // #5 Gamma and Beta adjust
                        // y_i = gamma*x_hat + beta
                        out_ptr[index] = scale_ptr[adjIndex] * inhat + bias_ptr[adjIndex];
                    } // end for(n_batchs)
                }     // for (column)
            }         // for (row)
        }             // for (channel)
    }
    return (ret);
}

template <typename _Tgpu /* the data type used in GPU computations (usually half) */,
          typename _Tcheck /* the data type used in CPU checkings (usually double) */>
void miopenBNActiveNeuronFwdInferHost(int neuron_type,
                                      _Tcheck gamma,
                                      _Tcheck beta,
                                      _Tcheck alpha,
                                      size_t size,
                                      const _Tgpu* bot_ptr,
                                      _Tcheck* c_res)
{

    _Tcheck* data = new _Tcheck[size];

    for(size_t k = 0; k < size; k++)
        data[k]  = static_cast<_Tcheck>(bot_ptr[k]);

    std::function<_Tcheck(_Tcheck)> f;

    switch(neuron_type)
    {
    case MIOPEN_NEURON_PASTHRU: //	x
        f = [=](_Tcheck x) { return x; };
        break;
    case MIOPEN_NEURON_LOGISTIC: //	1 / (1 + e^-x)	//Sigmoid
        f = [=](_Tcheck x) { return 1 / (1 + std::exp(-x)); };
        break;
    case MIOPEN_NEURON_TANH: //	beta * tanh(alpha * x)
        f = [=](_Tcheck x) { return beta * std::tanh(alpha * x); };
        break;
    case MIOPEN_NEURON_RELU: //	max(0, x)
        f = [=](_Tcheck x) { return (x > 0) ? x : 0; };
        break;
    case MIOPEN_NEURON_SOFTRELU: //	log(1 + e^x)   // bonomial normal log likelihood
        f = [=](_Tcheck x) { return std::log1p(std::exp(x)); };
        break;
    case MIOPEN_NEURON_ABS: //	abs(x)
        f = [=](_Tcheck x) { return std::abs(x); };
        break;
    case MIOPEN_NEURON_POWER: // (alpha + beta * x) ^ gamma
        f = [=](_Tcheck x) {
            _Tcheck v = alpha + beta * x;
            return v <= std::numeric_limits<_Tcheck>::epsilon() ? 0 : pow(v, gamma);
        };
        break;
    case MIOPEN_NEURON_CLIPPED_RELU: // min(alpha, max(0, x))
        f = [=](_Tcheck x) { return std::min(alpha, std::max(_Tcheck(0), x)); };
        break;
    case MIOPEN_NEURON_LEAKY_RELU: // alpha * x | x<=0; x | x>0
        f = [=](_Tcheck x) { return (x > 0) ? x : x * alpha; };
        break;
    case MIOPEN_NEURON_ELU: // alpah * (exp(x)-1) | x<=0; x | x>0
        f = [=](_Tcheck x) { return (x > 0) ? x : alpha * std::expm1(x); };
        break;
    default: printf("ERROR: unknown neuron type: %d\n", neuron_type); break;
    }

    for(size_t i = 0; i < size; i++)
        c_res[i] = f(data[i]);

    if(data)
    {
        delete[] data;
    }
}

template <typename _Tgpu /* the data type used in GPU computations (usually half) */,
          typename _Tcheck /* the data type used in CPU checkings (usually double) */>
int miopenBNActiveFwdInferVerify(size_t size,
                                 const _Tcheck* c_res,
                                 const _Tgpu* top_ptr,
                                 _Tcheck allowedEps)
{
    int match = 1;
    for(size_t i = 0; i < size && match; i++)
    {
        _Tcheck c_val  = c_res[i];
        _Tcheck g_val  = static_cast<_Tcheck>(top_ptr[i]);
        double err     = std::abs(c_val - g_val);
        double err_rel = calculate_relative_error(c_val, g_val);

        if((err > allowedEps && err_rel > allowedEps) || std::isnan(c_val) || std::isnan(g_val) ||
           !std::isfinite(c_val) || !std::isfinite(g_val))
        {
            std::cout << "Difference in neuron layer: " << err << " too large at " << i
                      << " c_v = " << c_val << " vs g_val = " << g_val
                      << " tolerance = " << allowedEps << std::endl;
            match = 0;
        }
    }

    return (match);
}

template <typename Tgpu, typename Tref>
int ConvForwardCPU(std::vector<Tgpu> in,
                   std::vector<Tgpu> outhost,
                   std::vector<Tgpu> wei,
                   std::vector<Tgpu> b,
                   int bias,
                   miopenConvolutionDescriptor_t convDesc,
                   miopenTensorDescriptor_t inputTensor,
                   miopenTensorDescriptor_t weightTensor,
                   miopenTensorDescriptor_t outputTensor)
{

    int in_n, in_c, in_h, in_w;
    int in_nstride, in_cstride, in_hstride, in_wstride;
    miopenDataType_t dt;
    miopenGet4dTensorDescriptor(inputTensor,
                                &dt,
                                &in_n,
                                &in_c,
                                &in_h,
                                &in_w,
                                &in_nstride,
                                &in_cstride,
                                &in_hstride,
                                &in_wstride);

    int wei_n, wei_c, wei_h, wei_w;
    int wei_nstride, wei_cstride, wei_hstride, wei_wstride;

    miopenGet4dTensorDescriptor(weightTensor,
                                &dt,
                                &wei_n,
                                &wei_c,
                                &wei_h,
                                &wei_w,
                                &wei_nstride,
                                &wei_cstride,
                                &wei_hstride,
                                &wei_wstride);

    int out_n, out_c, out_h, out_w;
    int out_nstride, out_cstride, out_hstride, out_wstride;
    miopenGet4dTensorDescriptor(outputTensor,
                                &dt,
                                &out_n,
                                &out_c,
                                &out_h,
                                &out_w,
                                &out_nstride,
                                &out_cstride,
                                &out_hstride,
                                &out_wstride);

    int u, v, pad_h, pad_w, dilation_h, dilation_w;
    miopenConvolutionMode_t mode;
    miopenPaddingMode_t pmode = miopen::deref(convDesc).paddingMode;
    miopenGetConvolutionDescriptor(
        convDesc, &mode, &pad_h, &pad_w, &u, &v, &dilation_h, &dilation_w);

    if(pmode == miopenPaddingSame)
    {
        pad_h = (in_h % u == 0) ? (std::max((wei_h - u), 0)) : (std::max((wei_h - (in_h % u)), 0));
        pad_w = (in_w % v == 0) ? (std::max((wei_w - v), 0)) : (std::max((wei_w - (in_w % v)), 0));
        pad_h /= 2;
        pad_w /= 2;
    }
    else if(pmode == miopenPaddingValid)
    {
        pad_h = 0;
        pad_w = 0;
    }

    if(out_h <= 0 || out_w <= 0)
        MIOPEN_THROW("Invalid Test Case: Check Output Dimension.");

    miopenGet4dTensorDescriptor(weightTensor,
                                &dt,
                                &wei_n,
                                &wei_c,
                                &wei_h,
                                &wei_w,
                                &wei_nstride,
                                &wei_cstride,
                                &wei_hstride,
                                &wei_wstride);

    for(int o = 0; o < out_n; o++)
    { // mini-batch size
        for(int w = 0; w < out_c; w++)
        { // out_channels (num filters)
            for(int i = 0; i < out_h; i++)
            { // output_height (from getforwardoutputdim())
                int in_off_h = i * u;
                for(int j = 0; j < out_w; j++)
                { // output_width (from getforwardoutputdim())
                    Tgpu acc     = static_cast<Tgpu>(0);
                    int in_off_w = j * v;
                    for(int k = 0; k < in_c; k++)
                    { // in_channels (RGB)
                        for(int x = 0; x < wei_h; x++)
                        {
                            int in_x = in_off_h - pad_h + x * dilation_h;
                            if(in_x >= 0 && in_x < in_h)
                            {
                                for(int y = 0; y < wei_w; y++)
                                {
                                    int in_y = in_off_w - pad_w + y * dilation_w;
                                    if(in_y >= 0 && in_y < in_w)
                                    {
                                        acc +=
                                            static_cast<Tgpu>(in[o * in_nstride + k * in_cstride +
                                                                 in_x * in_w + in_y]) *
                                            static_cast<Tgpu>(
                                                wei[w * wei_nstride + k * wei_cstride +
                                                    x * wei_hstride + y]);
                                    }
                                }
                            }
                        }
                    }
                    acc = bias != 0 ? acc + static_cast<Tgpu>(b[w]) : acc;
                    outhost[o * out_nstride + w * out_cstride + i * out_hstride + j] = acc;
                }
            }
        }
    }

    return 0;
}

template <typename Tgpu, typename Tref>
int miopenBNActiveVerify(InputFlags inflags,
                         miopenTensorDescriptor_t inputTensor,
                         miopenActivationDescriptor_t activDesc,
                         Tref epsilon,
                         double* estimatedMean,
                         double* estimatedVariance,
                         std::vector<Tgpu> in,
                         std::vector<Tgpu> bn_res,
                         std::vector<Tgpu> scale,
                         std::vector<Tgpu> bias,
                         std::vector<Tgpu> out)
{
    miopenBatchNormMode_t bn_mode;
    if(inflags.GetValueInt("bnMode") == 0)
    {
        bn_mode = miopenBNPerActivation;
    }
    else if(inflags.GetValueInt("bnMode") == 1)
    {
        bn_mode = miopenBNSpatial;
    }
    else
    {
        printf("Incorrect Batch Normalization Mode\n");
        exit(EXIT_FAILURE);
    }

    double activ_alpha, activ_beta, activ_gamma;
    miopenActivationMode_t activ_mode;
    miopenGetActivationDescriptor(activDesc, &activ_mode, &activ_alpha, &activ_beta, &activ_gamma);

    if(bn_mode == miopenBNPerActivation)
    { // 1xCxHxW
        miopenBNActiveBNPerActivFwdInferHost(inputTensor,
                                             in.data(),
                                             bn_res.data(),
                                             scale.data(),
                                             bias.data(),
                                             epsilon,
                                             true,
                                             estimatedMean,
                                             estimatedVariance);
    }
    else if(bn_mode == miopenBNSpatial)
    { // 1xCx1x1
        miopenBNActiveBNSpatialFwdInferHost(inputTensor,
                                            in.data(),
                                            bn_res.data(),
                                            scale.data(),
                                            bias.data(),
                                            epsilon,
                                            true,
                                            estimatedMean,
                                            estimatedVariance);
    }

    Tref* c_res = new Tref[out.size()];
    miopenBNActiveNeuronFwdInferHost<Tgpu, Tref>(
        activ_mode, activ_gamma, activ_beta, activ_alpha, out.size(), bn_res.data(), c_res);

    double allowedEps = std::numeric_limits<Tgpu>::epsilon() * 80;

    int match = miopenBNActiveFwdInferVerify<Tgpu, Tref>(out.size(), c_res, out.data(), allowedEps);

    return match;
}

#endif
