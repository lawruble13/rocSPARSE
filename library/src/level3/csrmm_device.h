/*! \file */
/* ************************************************************************
 * Copyright (c) 2018-2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ************************************************************************ */

#pragma once
#ifndef CSRMM_DEVICE_H
#define CSRMM_DEVICE_H

#include "common.h"

template <unsigned int BLOCKSIZE, unsigned int WF_SIZE, typename I, typename J, typename T>
static __device__ void csrmmnn_general_device(rocsparse_operation trans_A,
                                              rocsparse_operation trans_B,
                                              J                   M,
                                              J                   N,
                                              J                   K,
                                              I                   nnz,
                                              T                   alpha,
                                              const I* __restrict__ csr_row_ptr,
                                              const J* __restrict__ csr_col_ind,
                                              const T* __restrict__ csr_val,
                                              const T* __restrict__ B,
                                              J ldb,
                                              T beta,
                                              T* __restrict__ C,
                                              J                    ldc,
                                              rocsparse_order      order,
                                              rocsparse_index_base idx_base)
{
    int tid = hipThreadIdx_x;
    J   gid = hipBlockIdx_x * BLOCKSIZE + tid;
    int lid = gid & (WF_SIZE - 1);
    int wid = tid / WF_SIZE;
    J   nwf = hipGridDim_x * BLOCKSIZE / WF_SIZE;
    J   col = lid + hipBlockIdx_y * WF_SIZE;

    J colB = col * ldb;

    __shared__ J shared_col[BLOCKSIZE / WF_SIZE][WF_SIZE];
    __shared__ T shared_val[BLOCKSIZE / WF_SIZE][WF_SIZE];

    for(J row = gid / WF_SIZE; row < M; row += nwf)
    {
        I row_start = csr_row_ptr[row] - idx_base;
        I row_end   = csr_row_ptr[row + 1] - idx_base;

        T sum = static_cast<T>(0);

        for(I j = row_start; j < row_end; j += WF_SIZE)
        {
            I k = j + lid;

            __syncthreads();

            shared_col[wid][lid] = (k < row_end) ? csr_col_ind[k] - idx_base : 0;

            if(trans_A == rocsparse_operation_conjugate_transpose)
            {
                shared_val[wid][lid]
                    = (k < row_end) ? rocsparse_conj(csr_val[k]) : static_cast<T>(0);
            }
            else
            {
                shared_val[wid][lid] = (k < row_end) ? csr_val[k] : static_cast<T>(0);
            }

            __syncthreads();

            for(J i = 0; i < WF_SIZE && col < N; ++i)
            {
                if(trans_B == rocsparse_operation_conjugate_transpose)
                {
                    sum = rocsparse_fma(
                        shared_val[wid][i], rocsparse_conj(B[shared_col[wid][i] + colB]), sum);
                }
                else
                {
                    sum = rocsparse_fma(shared_val[wid][i], B[shared_col[wid][i] + colB], sum);
                }
            }
        }

        if(col < N)
        {
            if(beta == static_cast<T>(0))
            {
                if(order == rocsparse_order_column)
                {
                    C[row + col * ldc] = alpha * sum;
                }
                else
                {
                    C[row * ldc + col] = alpha * sum;
                }
            }
            else
            {
                if(order == rocsparse_order_column)
                {
                    C[row + col * ldc] = rocsparse_fma(beta, C[row + col * ldc], alpha * sum);
                }
                else
                {
                    C[row * ldc + col] = rocsparse_fma(beta, C[row * ldc + col], alpha * sum);
                }
            }
        }
    }
}

template <unsigned int BLOCKSIZE, unsigned int WF_SIZE, typename I, typename J, typename T>
static __device__ void csrmmnt_general_device(rocsparse_operation trans_A,
                                              rocsparse_operation trans_B,
                                              J                   offset,
                                              J                   ncol,
                                              J                   M,
                                              J                   N,
                                              J                   K,
                                              I                   nnz,
                                              T                   alpha,
                                              const I* __restrict__ csr_row_ptr,
                                              const J* __restrict__ csr_col_ind,
                                              const T* __restrict__ csr_val,
                                              const T* __restrict__ B,
                                              J ldb,
                                              T beta,
                                              T* __restrict__ C,
                                              J                    ldc,
                                              rocsparse_order      order,
                                              rocsparse_index_base idx_base)
{
    int tid = hipThreadIdx_x;
    J   gid = hipBlockIdx_x * BLOCKSIZE + tid;
    J   row = gid / WF_SIZE;
    int lid = tid & (WF_SIZE - 1);
    int wid = tid / WF_SIZE;

    if(row >= M)
    {
        return;
    }

    __shared__ J shared_col[BLOCKSIZE / WF_SIZE][WF_SIZE];
    __shared__ T shared_val[BLOCKSIZE / WF_SIZE][WF_SIZE];

    I row_start = csr_row_ptr[row] - idx_base;
    I row_end   = csr_row_ptr[row + 1] - idx_base;

    for(J l = offset; l < ncol; l += WF_SIZE)
    {
        J col = l + lid;
        T sum = static_cast<T>(0);

        for(I j = row_start; j < row_end; j += WF_SIZE)
        {
            I k = j + lid;

            __syncthreads();

            shared_col[wid][lid] = (k < row_end) ? ldb * (csr_col_ind[k] - idx_base) : 0;

            if(trans_A == rocsparse_operation_conjugate_transpose)
            {
                shared_val[wid][lid]
                    = (k < row_end) ? rocsparse_conj(csr_val[k]) : static_cast<T>(0);
            }
            else
            {
                shared_val[wid][lid] = (k < row_end) ? csr_val[k] : static_cast<T>(0);
            }

            __syncthreads();

            for(J i = 0; i < WF_SIZE; ++i)
            {
                if(trans_B == rocsparse_operation_conjugate_transpose)
                {
                    T val_B = (col < ncol) ? rocsparse_ldg(B + col + shared_col[wid][i])
                                           : static_cast<T>(0);
                    sum     = rocsparse_fma(shared_val[wid][i], rocsparse_conj(val_B), sum);
                }
                else
                {
                    T val_B = (col < ncol) ? rocsparse_ldg(B + col + shared_col[wid][i])
                                           : static_cast<T>(0);
                    sum     = rocsparse_fma(shared_val[wid][i], val_B, sum);
                }
            }
        }

        if(col < ncol)
        {
            if(beta == static_cast<T>(0))
            {
                if(order == rocsparse_order_column)
                {
                    C[row + col * ldc] = alpha * sum;
                }
                else
                {
                    C[row * ldc + col] = alpha * sum;
                }
            }
            else
            {
                if(order == rocsparse_order_column)
                {
                    C[row + col * ldc] = rocsparse_fma(beta, C[row + col * ldc], alpha * sum);
                }
                else
                {
                    C[row * ldc + col] = rocsparse_fma(beta, C[row * ldc + col], alpha * sum);
                }
            }
        }
    }
}

// Scale kernel for beta != 1.0
template <typename I, typename T>
static __device__ void
    csrmm_scale_device(I m, I n, T beta, T* __restrict__ data, I ld, rocsparse_order order)
{
    I gidx = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
    I gidy = hipBlockIdx_y * hipBlockDim_y + hipThreadIdx_y;

    if(gidx >= m || gidy >= n)
    {
        return;
    }

    if(order == rocsparse_order_column)
    {
        data[gidx + ld * gidy] = data[gidx + ld * gidy] * beta;
    }
    else
    {
        data[gidy + ld * gidx] = data[gidy + ld * gidx] * beta;
    }
}

// See Y. Tao et al., "Atomic reduction based sparse matrix-transpose vector multiplication on GPUs,"
// 2014 20th IEEE International Conference on Parallel and Distributed Systems (ICPADS), 2014, pp. 987-992,
// doi: 10.1109/PADSW.2014.7097920.
template <unsigned int BLOCKSIZE, unsigned int WF_SIZE, typename I, typename J, typename T>
static __device__ void csrmmtn_general_device(rocsparse_operation trans_A,
                                              rocsparse_operation trans_B,
                                              J                   M,
                                              J                   N,
                                              J                   K,
                                              I                   nnz,
                                              T                   alpha,
                                              const I* __restrict__ csr_row_ptr,
                                              const J* __restrict__ csr_col_ind,
                                              const T* __restrict__ csr_val,
                                              const T* __restrict__ B,
                                              J ldb,
                                              T beta,
                                              T* __restrict__ C,
                                              J                    ldc,
                                              rocsparse_order      order,
                                              rocsparse_index_base idx_base)
{
    int tid = hipThreadIdx_x;
    J   gid = hipBlockIdx_x * BLOCKSIZE + tid;
    int lid = gid & (WF_SIZE - 1);
    int wid = tid / WF_SIZE;

    J nwf = hipGridDim_x * BLOCKSIZE / WF_SIZE;

    J cid  = lid + hipBlockIdx_y * WF_SIZE;
    J colB = cid * ldb;

    __shared__ T shared_B[BLOCKSIZE / WF_SIZE][WF_SIZE];

    for(J row = gid / WF_SIZE; row < K; row += nwf)
    {
        I row_start = csr_row_ptr[row] - idx_base;
        I row_end   = csr_row_ptr[row + 1] - idx_base;

        if(trans_B == rocsparse_operation_conjugate_transpose)
        {
            shared_B[wid][lid] = (cid < N) ? rocsparse_conj(B[row + colB]) : static_cast<T>(0);
        }
        else
        {
            shared_B[wid][lid] = (cid < N) ? B[row + colB] : static_cast<T>(0);
        }

        __syncthreads();

        for(I j = row_start + lid; j < row_end; j += WF_SIZE)
        {
            J col = csr_col_ind[j] - idx_base;
            T val = static_cast<T>(0);
            if(trans_A == rocsparse_operation_conjugate_transpose)
            {
                val = alpha * rocsparse_conj(csr_val[j]);
            }
            else
            {
                val = alpha * csr_val[j];
            }

            if(order == rocsparse_order_column)
            {
                for(J i = 0; i < WF_SIZE && (i + hipBlockIdx_y * WF_SIZE) < N; ++i)
                {
                    atomicAdd(&C[col + (i + hipBlockIdx_y * WF_SIZE) * ldc],
                              val * shared_B[wid][i]);
                }
            }
            else
            {
                for(J i = 0; i < WF_SIZE && (i + hipBlockIdx_y * WF_SIZE) < N; ++i)
                {
                    atomicAdd(&C[col * ldc + (i + hipBlockIdx_y * WF_SIZE)],
                              val * shared_B[wid][i]);
                }
            }
        }
    }
}

// See Y. Tao et al., "Atomic reduction based sparse matrix-transpose vector multiplication on GPUs,"
// 2014 20th IEEE International Conference on Parallel and Distributed Systems (ICPADS), 2014, pp. 987-992,
// doi: 10.1109/PADSW.2014.7097920.
template <unsigned int BLOCKSIZE, unsigned int WF_SIZE, typename I, typename J, typename T>
static __device__ void csrmmtt_general_device(rocsparse_operation trans_A,
                                              rocsparse_operation trans_B,
                                              J                   M,
                                              J                   N,
                                              J                   K,
                                              I                   nnz,
                                              T                   alpha,
                                              const I* __restrict__ csr_row_ptr,
                                              const J* __restrict__ csr_col_ind,
                                              const T* __restrict__ csr_val,
                                              const T* __restrict__ B,
                                              J ldb,
                                              T beta,
                                              T* __restrict__ C,
                                              J                    ldc,
                                              rocsparse_order      order,
                                              rocsparse_index_base idx_base)
{
    int tid = hipThreadIdx_x;
    J   gid = hipBlockIdx_x * BLOCKSIZE + tid;
    int lid = gid & (WF_SIZE - 1);
    int wid = tid / WF_SIZE;

    J nwf = hipGridDim_x * BLOCKSIZE / WF_SIZE;

    J cid = lid + hipBlockIdx_y * WF_SIZE;

    __shared__ T shared_B[BLOCKSIZE / WF_SIZE][WF_SIZE];

    for(J row = gid / WF_SIZE; row < K; row += nwf)
    {
        I row_start = csr_row_ptr[row] - idx_base;
        I row_end   = csr_row_ptr[row + 1] - idx_base;

        if(trans_B == rocsparse_operation_conjugate_transpose)
        {
            shared_B[wid][lid] = (cid < N) ? rocsparse_conj(B[ldb * row + cid]) : static_cast<T>(0);
        }
        else
        {
            shared_B[wid][lid] = (cid < N) ? B[ldb * row + cid] : static_cast<T>(0);
        }

        __syncthreads();

        for(I j = row_start + lid; j < row_end; j += WF_SIZE)
        {
            J col = csr_col_ind[j] - idx_base;
            T val = static_cast<T>(0);
            if(trans_A == rocsparse_operation_conjugate_transpose)
            {
                val = alpha * rocsparse_conj(csr_val[j]);
            }
            else
            {
                val = alpha * csr_val[j];
            }

            if(order == rocsparse_order_column)
            {
                for(J i = 0; i < WF_SIZE && (i + hipBlockIdx_y * WF_SIZE) < N; ++i)
                {
                    atomicAdd(&C[col + (i + hipBlockIdx_y * WF_SIZE) * ldc],
                              val * shared_B[wid][i]);
                }
            }
            else
            {
                for(J i = 0; i < WF_SIZE && (i + hipBlockIdx_y * WF_SIZE) < N; ++i)
                {
                    atomicAdd(&C[col * ldc + (i + hipBlockIdx_y * WF_SIZE)],
                              val * shared_B[wid][i]);
                }
            }
        }
    }
}

#endif // CSRMM_DEVICE_H
