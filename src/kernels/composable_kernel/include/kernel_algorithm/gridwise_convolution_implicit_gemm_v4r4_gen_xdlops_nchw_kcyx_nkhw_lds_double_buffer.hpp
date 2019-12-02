#ifndef CK_GRIDWISE_CONVOLUTION_IMPLICIT_GEMM_V4R4_NCHW_KCYX_NKHW_LDS_DOUBLE_BUFFER_HPP
#define CK_GRIDWISE_CONVOLUTION_IMPLICIT_GEMM_V4R4_NCHW_KCYX_NKHW_LDS_DOUBLE_BUFFER_HPP

#include "common_header.hpp"
#include "tensor_descriptor.hpp"
#include "tensor_descriptor_helper.hpp"
#include "ConstantMatrixDescriptor.hpp"
#include "blockwise_generic_tensor_slice_copy.hpp"
#include "threadwise_generic_tensor_slice_copy.hpp"
#include "blockwise_gemm_xdlops.hpp"
#include "convolution_common.hpp"
#include "implicitgemm_params.hpp"

namespace ck {

template <ImplicitGemmDirection conv_dir>
struct make_WeiDesc_Xdlops;

template <>
struct make_WeiDesc_Xdlops<ImplicitGemmDirection::ForwardData>
{
    template <typename WeiDesc>
    __device__ constexpr auto get(WeiDesc&)
    {
        constexpr auto I1 = Number<1>{};
        constexpr auto I3 = Number<3>{};

        return reorder_tensor_descriptor_given_upper2lower(
            unfold_tensor_descriptor(WeiDesc{}, I1, I3), Sequence<1, 0>{});
    }
};

template <>
struct make_WeiDesc_Xdlops<ImplicitGemmDirection::BackwardWeight>
{
    template <typename WeiDesc>
    __device__ constexpr auto get(WeiDesc& desc)
    {

        constexpr auto I0 = Number<0>{};
        constexpr auto I1 = Number<1>{};
        constexpr auto I2 = Number<2>{};
        constexpr auto I3 = Number<3>{};

        constexpr auto wei_k_c_y_x_global_desc = WeiDesc{};

        constexpr index_t K = wei_k_c_y_x_global_desc.GetLength(I0);
        constexpr index_t C = wei_k_c_y_x_global_desc.GetLength(I1);
        constexpr index_t Y = wei_k_c_y_x_global_desc.GetLength(I2);
        constexpr index_t X = wei_k_c_y_x_global_desc.GetLength(I3);

        return transform_tensor_descriptor(
            unfold_tensor_descriptor(WeiDesc{}, I2, I3),
            make_tuple(Merge<Sequence<C, Y * X>>{}, PassThrough<K>{}),
            make_tuple(Sequence<1, 2>{}, Sequence<0>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}));
    }
};

// B = merge(N, Ho, Wo)
template <index_t GridSize,
          index_t BlockSize,
          class Float,
          class AccDataType,
          class InGlobalDesc,
          class WeiGlobalDesc,
          class OutGlobalDesc,
          class ConvStrides,
          class ConvDilations,
          class LeftPads,
          class RightPads,
          index_t BPerBlock,
          index_t KPerBlock,
          index_t EPerBlock,
          index_t GemmMPerWave,
          index_t GemmNPerWave,
          index_t GemmMWaves,
          index_t GemmNWaves,
          index_t GemmDataPerReadA,
          index_t GemmDataPerReadB,
          class InBlockCopySubLengths_E_B,
          class InBlockCopyClusterLengths_E_B,
          class InBlockCopyThreadClusterArrangeOrder,
          class InBlockCopySrcAccessOrder,
          class InBlockCopyDstAccessOrder,
          index_t InBlockCopySrcDataPerRead_B,
          index_t InBlockCopyDstDataPerWrite_B,
          class WeiBlockCopySubLengths_E_K,
          class WeiBlockCopyClusterLengths_E_K,
          class WeiBlockCopyThreadClusterArrangeOrder,
          class WeiBlockCopySrcAccessOrder,
          class WeiBlockCopyDstAccessOrder,
          index_t WeiBlockCopySrcDataPerRead_E,
          index_t WeiBlockCopyDstDataPerWrite_K,
          index_t OutThreadCopyDataPerAccess_B,
          ImplicitGemmDirection conv_dir>
struct GridwiseConvolutionImplicitGemm_v4r4_gen_xdlops_nchw_kcyx_nkhw_lds_double_buffer
{
    __device__ void Run(const Float* const __restrict__ p_in_global,
                        const Float* const __restrict__ p_wei_global,
                        Float* const __restrict__ p_out_global) const
    {
        constexpr auto I0 = Number<0>{};
        constexpr auto I1 = Number<1>{};
        constexpr auto I2 = Number<2>{};
        constexpr auto I3 = Number<3>{};

        constexpr auto True = integral_constant<bool, true>{};

        constexpr auto generic_address_space =
            integral_constant<AddressSpace, AddressSpace::generic>{};
        constexpr auto global_address_space =
            integral_constant<AddressSpace, AddressSpace::global>{};

        constexpr auto in_n_c_hi_wi_global_desc  = InGlobalDesc{};
        constexpr auto wei_k_c_y_x_global_desc   = WeiGlobalDesc{};
        constexpr auto out_n_k_ho_wo_global_desc = OutGlobalDesc{};

        constexpr index_t N  = in_n_c_hi_wi_global_desc.GetLength(I0);
        constexpr index_t C  = in_n_c_hi_wi_global_desc.GetLength(I1);
        constexpr index_t Hi = in_n_c_hi_wi_global_desc.GetLength(I2);
        constexpr index_t Wi = in_n_c_hi_wi_global_desc.GetLength(I3);

        constexpr index_t K  = out_n_k_ho_wo_global_desc.GetLength(I1);
        constexpr index_t Ho = out_n_k_ho_wo_global_desc.GetLength(I2);
        constexpr index_t Wo = out_n_k_ho_wo_global_desc.GetLength(I3);

        constexpr index_t Y = wei_k_c_y_x_global_desc.GetLength(I2);
        constexpr index_t X = wei_k_c_y_x_global_desc.GetLength(I3);

        constexpr index_t ConvStrideH = ConvStrides{}[0];
        constexpr index_t ConvStrideW = ConvStrides{}[1];

        constexpr index_t ConvDilationH = ConvDilations{}[0];
        constexpr index_t ConvDilationW = ConvDilations{}[1];

        constexpr index_t E = C * Y * X;
        constexpr index_t B = N * Ho * Wo;

        // sanity-check for vectorized memory load
        static_assert((Wo == 1 || (ConvStrideW == 1 || InBlockCopySrcDataPerRead_B == 1)) &&
                          (X == 1 || ConvDilationW % InBlockCopySrcDataPerRead_B == 0),
                      "wrong! aligment requirement for vectorized global load of input tensor will "
                      "be violated");

        // divide block work by [K, B]
        static_assert(K % KPerBlock == 0 && B % BPerBlock == 0 && E % EPerBlock == 0,
                      "wrong! cannot divide work evenly among block");

        constexpr index_t KBlockWork = K / KPerBlock;
        constexpr index_t BBlockWork = B / BPerBlock;

        constexpr auto block_work_desc =
            make_cluster_descriptor(Sequence<KBlockWork, BBlockWork>{});

        const auto block_work_id = block_work_desc.CalculateClusterIndex(get_block_1d_id());

        const index_t k_block_data_on_global = block_work_id[0] * KPerBlock;
        const index_t b_block_data_on_global = block_work_id[1] * BPerBlock;

        // input tensor
        //   global mem
        constexpr auto in_n_c_hip_wip_global_desc = transform_tensor_descriptor(
            in_n_c_hi_wi_global_desc,
            make_tuple(
                PassThrough<N>{}, PassThrough<C>{}, Pad<Sequence<Hi, Wi>, LeftPads, RightPads>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2, 3>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2, 3>{}));

        constexpr auto in_n_c_y_ho_x_wo_global_desc = transform_tensor_descriptor(
            in_n_c_hip_wip_global_desc,
            make_tuple(PassThrough<N>{},
                       PassThrough<C>{},
                       Embed<Sequence<Y, Ho>, Sequence<ConvDilationH, ConvStrideH, 0>>{},
                       Embed<Sequence<X, Wo>, Sequence<ConvDilationW, ConvStrideW, 0>>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2>{}, Sequence<3>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2, 3>{}, Sequence<4, 5>{}));

        constexpr auto in_e_b_global_desc = transform_tensor_descriptor(
            in_n_c_y_ho_x_wo_global_desc,
            make_tuple(Merge<Sequence<C, Y, X>>{}, Merge<Sequence<N, Ho, Wo>>{}),
            make_tuple(Sequence<1, 2, 4>{}, Sequence<0, 3, 5>{}),
            make_tuple(Sequence<0>{}, Sequence<1>{}));

        //   LDS mem
        //     be careful of LDS alignment
        // LDS allocation for input and weight: be careful of alignment
        constexpr index_t max_align = math::lcm(InBlockCopyDstDataPerWrite_B,
                                                WeiBlockCopyDstDataPerWrite_K,
                                                GemmDataPerReadA,
                                                GemmDataPerReadB);
        constexpr auto in_e_b_block_desc = make_native_tensor_descriptor_aligned(
            Sequence<EPerBlock, BPerBlock>{}, Number<max_align>{});

        // input blockwise copy
        auto blockwise_in_copy =
            BlockwiseGenericTensorSliceCopy_v4<BlockSize,
                                               decltype(in_e_b_global_desc),
                                               decltype(in_e_b_block_desc),
                                               decltype(in_e_b_block_desc.GetLengths()),
                                               InBlockCopySubLengths_E_B,
                                               InBlockCopyClusterLengths_E_B,
                                               InBlockCopyThreadClusterArrangeOrder,
                                               InBlockCopySrcAccessOrder,
                                               InBlockCopyDstAccessOrder,
                                               1,
                                               1,
                                               InBlockCopySrcDataPerRead_B,
                                               InBlockCopyDstDataPerWrite_B>(
                {0, b_block_data_on_global}, {0, 0});

        // weight tensor
        //   global mem
        constexpr auto wei_e_k_global_desc =
            make_WeiDesc_Xdlops<conv_dir>{}.get(wei_k_c_y_x_global_desc);

        //   LDS
        //     be careful of LDS alignment
        constexpr auto wei_e_k_block_desc = make_native_tensor_descriptor_aligned(
            Sequence<EPerBlock, KPerBlock>{}, Number<max_align>{});

        // weight blockwise copy
        auto blockwise_wei_copy =
            BlockwiseGenericTensorSliceCopy_v4<BlockSize,
                                               decltype(wei_e_k_global_desc),
                                               decltype(wei_e_k_block_desc),
                                               decltype(wei_e_k_block_desc.GetLengths()),
                                               WeiBlockCopySubLengths_E_K,
                                               WeiBlockCopyClusterLengths_E_K,
                                               WeiBlockCopyThreadClusterArrangeOrder,
                                               WeiBlockCopySrcAccessOrder,
                                               WeiBlockCopyDstAccessOrder,
                                               0,
                                               1,
                                               WeiBlockCopySrcDataPerRead_E,
                                               WeiBlockCopyDstDataPerWrite_K>(
                {0, k_block_data_on_global}, {0, 0});

        // GEMM definition
        // c_mtx += transpose(a_mtx) * b_mtx
        //     a_mtx[EPerBlock, KPerBlock] is in LDS
        //     b_mtx[EPerBlocl, BPerBlock] is in LDS
        //     c_mtx[KPerBlock, BPerBlock] is distributed among threads, and saved in
        //     register
        constexpr auto a_e_k_block_mtx_desc = make_ConstantMatrixDescriptor(wei_e_k_block_desc);
        constexpr auto b_e_b_block_mtx_desc = make_ConstantMatrixDescriptor(in_e_b_block_desc);

        const auto blockwise_gemm = BlockwiseGemmBlockABlockBThreadCTransANormalBNormalC_xdlops<
            BlockSize,
            decltype(a_e_k_block_mtx_desc),
            decltype(b_e_b_block_mtx_desc),
            Float,
            GemmMPerWave,
            GemmNPerWave,
            GemmMWaves,
            GemmNWaves,
            GemmDataPerReadA,
            GemmDataPerReadB>{};

        constexpr auto c_k_thread_mtx_desc = blockwise_gemm.GetThreadMatrixCDescriptor();

        constexpr index_t in_block_space =
            math::integer_least_multiple(in_e_b_block_desc.GetElementSpace(), max_align);

        constexpr index_t wei_block_space =
            math::integer_least_multiple(wei_e_k_block_desc.GetElementSpace(), max_align);

        __shared__ Float p_in_block_double[2 * in_block_space];
        __shared__ Float p_wei_block_double[2 * wei_block_space];

        // register allocation for output
        AccDataType p_out_thread[c_k_thread_mtx_desc.GetElementSpace()];

        // zero out threadwise output
        threadwise_matrix_set_zero(c_k_thread_mtx_desc, p_out_thread);
        blockwise_gemm.XdlopsMatrixCSetZero();

        // LDS double buffer: preload data into LDS
        {
            blockwise_in_copy.Run(
                p_in_global, p_in_block_double, global_address_space, generic_address_space);
            blockwise_wei_copy.Run(
                p_wei_global, p_wei_block_double, global_address_space, generic_address_space);
        }

        // LDS double buffer: main body
        for(index_t e_block_data_begin = 0; e_block_data_begin + 2 * EPerBlock < E;
            e_block_data_begin += 2 * EPerBlock)
        {
#pragma unroll
            for(index_t iloop = 0; iloop < 2; ++iloop)
            {
                const bool even_loop = (iloop % 2 == 0);

                Float* p_in_block_now =
                    even_loop ? p_in_block_double : p_in_block_double + in_block_space;
                Float* p_wei_block_now =
                    even_loop ? p_wei_block_double : p_wei_block_double + wei_block_space;

                Float* p_in_block_next =
                    even_loop ? p_in_block_double + in_block_space : p_in_block_double;
                Float* p_wei_block_next =
                    even_loop ? p_wei_block_double + wei_block_space : p_wei_block_double;

                Float p_in_thread_buffer[blockwise_in_copy.GetThreadBufferSize()];
                Float p_wei_thread_buffer[blockwise_wei_copy.GetThreadBufferSize()];

                blockwise_in_copy.MoveSrcSliceWindow(Sequence<EPerBlock, 0>{}, True);
                blockwise_wei_copy.MoveSrcSliceWindow(Sequence<EPerBlock, 0>{}, True);

                __syncthreads();

                // LDS doubel buffer: load next data from device mem
                blockwise_in_copy.RunLoadThreadBuffer(
                    p_in_global, p_in_thread_buffer, global_address_space, generic_address_space);
                blockwise_wei_copy.RunLoadThreadBuffer(
                    p_wei_global, p_wei_thread_buffer, global_address_space, generic_address_space);

                // LDS double buffer: GEMM on current data
                blockwise_gemm.Run(p_wei_block_now, p_in_block_now, p_out_thread);

                // LDS double buffer: store next data to LDS
                blockwise_in_copy.RunStoreThreadBuffer(p_in_thread_buffer, p_in_block_next);
                blockwise_wei_copy.RunStoreThreadBuffer(p_wei_thread_buffer, p_wei_block_next);
            }
        }

        // LDS double buffer: tail
        {
            constexpr bool has_two_iteration_left = (E % (2 * EPerBlock) == 0);

            if(has_two_iteration_left) // if has 2 iteration left
            {
                Float p_in_thread_buffer[blockwise_in_copy.GetThreadBufferSize()];
                Float p_wei_thread_buffer[blockwise_wei_copy.GetThreadBufferSize()];

                blockwise_in_copy.MoveSrcSliceWindow(Sequence<EPerBlock, 0>{}, True);
                blockwise_wei_copy.MoveSrcSliceWindow(Sequence<EPerBlock, 0>{}, True);

                __syncthreads();

                // LDS double buffer: load last data from device mem
                blockwise_in_copy.RunLoadThreadBuffer(
                    p_in_global, p_in_thread_buffer, global_address_space, generic_address_space);
                blockwise_wei_copy.RunLoadThreadBuffer(
                    p_wei_global, p_wei_thread_buffer, global_address_space, generic_address_space);

                // LDS double buffer: GEMM on 2nd-last data
                blockwise_gemm.Run(p_wei_block_double, p_in_block_double, p_out_thread);

                // LDS double buffer: store last data to LDS
                blockwise_in_copy.RunStoreThreadBuffer(p_in_thread_buffer,
                                                       p_in_block_double + in_block_space);
                blockwise_wei_copy.RunStoreThreadBuffer(p_wei_thread_buffer,
                                                        p_wei_block_double + wei_block_space);

                __syncthreads();

                // LDS double buffer: GEMM on current data
                blockwise_gemm.Run(p_wei_block_double + wei_block_space,
                                   p_in_block_double + in_block_space,
                                   p_out_thread);
            }
            else // if has 1 iteration left
            {
                __syncthreads();

                // LDS double buffer: GEMM on last data
                blockwise_gemm.Run(p_wei_block_double, p_in_block_double, p_out_thread);
            }
        }
        // load data from xldop_acc_regs
        blockwise_gemm.XdlopsMatrixCRead(p_out_thread);

        // copy output: register to global memory
        {
            constexpr auto OutputLayout = blockwise_gemm.GetOutputLayout();
            constexpr index_t K2        = OutputLayout.M1();
            constexpr index_t K1        = OutputLayout.N1();
            constexpr index_t K0        = OutputLayout.M0();

            constexpr auto out_k_b_global_desc = transform_tensor_descriptor(
                out_n_k_ho_wo_global_desc,
                make_tuple(PassThrough<K>{}, Merge<Sequence<N, Ho, Wo>>{}),
                make_tuple(Sequence<1>{}, Sequence<0, 2, 3>{}),
                make_tuple(Sequence<0>{}, Sequence<1>{}));

            constexpr auto out_k0_k1_k2_b_global_desc = transform_tensor_descriptor(
                out_k_b_global_desc,
                make_tuple(UnMerge<Sequence<K0, K1, K2>>{}, PassThrough<B>{}),
                make_tuple(Sequence<0>{}, Sequence<1>{}),
                make_tuple(Sequence<0, 1, 2>{}, Sequence<3>{}));

            //     src descriptor
            constexpr auto out_k0_k1_k2_b_thread_desc =
                make_native_tensor_descriptor_packed(Sequence<K2, 1, K0, 1>{});

            using OutThreadCopySliceLengths = Sequence<K2, 1, K0, 1>;

            constexpr index_t NumKPerBlk = OutputLayout.GetSizeM();
            static_assert(OutputLayout.GetSizeM() == 16, "MSize != 16");
            constexpr index_t NumBlks = c_k_thread_mtx_desc.GetElementSpace() / NumKPerBlk;

            for(index_t i = 0; i < NumBlks; ++i)
            {
                // calculate origin of thread output tensor on global memory
                //     blockwise GEMM c matrix starting index
                const auto c_thread_mtx_on_block = blockwise_gemm.GetBeginOfThreadMatrixC(i);

                const index_t k_thread_data_on_global =
                    k_block_data_on_global + c_thread_mtx_on_block.row;

                const index_t b_thread_data_on_global =
                    b_block_data_on_global + c_thread_mtx_on_block.col;

                ThreadwiseGenericTensorSliceCopy_v4r2<decltype(out_k0_k1_k2_b_thread_desc),
                                                      decltype(out_k0_k1_k2_b_global_desc),
                                                      OutThreadCopySliceLengths,
                                                      arithmetic_sequence_gen<0, 4, 1>::type,
                                                      3,
                                                      OutThreadCopyDataPerAccess_B,
                                                      OutThreadCopyDataPerAccess_B>(
                    {0, 0, 0, 0},
                    {k_thread_data_on_global / (K0 * K1),
                     k_thread_data_on_global % (K0 * K1) / K0,
                     k_thread_data_on_global % K0,
                     b_thread_data_on_global})
                    .Run(p_out_thread + i * NumKPerBlk,
                         p_out_global,
                         generic_address_space,
                         global_address_space);
            }
        }
    }
};

} // namespace ck
#endif