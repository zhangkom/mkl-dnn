/*******************************************************************************
* Copyright 2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef RNN_PD_HPP
#define RNN_PD_HPP

#include "mkldnn.h"

#include "c_types_map.hpp"
#include "memory_pd.hpp"
#include "primitive_desc.hpp"

namespace mkldnn {
namespace impl {

// struct rnn_fwd_pd_t;

struct rnn_pd_t : public primitive_desc_t {
    static constexpr auto base_pkind = primitive_kind::rnn;

    rnn_pd_t(mkldnn::impl::engine_t *engine, const rnn_desc_t *adesc,
            const primitive_attr_t *attr, const rnn_pd_t *hint_pd)
        : primitive_desc_t(engine, attr, primitive_kind::rnn)
        , desc_(*adesc)
        , hint_pd_(hint_pd) {}
    virtual ~rnn_pd_t() {}

    const rnn_desc_t *desc() const { return &desc_; }
    virtual const op_desc_t *op_desc() const override {
        return reinterpret_cast<const op_desc_t *>(this->desc());
    }
    virtual void init_info() override { init_info_rnn(this, this->info_); }

    virtual status_t query(query_t what, int idx, void *result) const override {
        switch (what) {
        case query::rnn_d: *(const rnn_desc_t **)result = desc(); break;
        default: return primitive_desc_t::query(what, idx, result);
        }
        return status::success;
    }

    inline bool is_training() const {
        return utils::one_of(desc_.prop_kind, prop_kind::forward_training,
                prop_kind::backward);
    }

    inline size_t ws_states_size() {
        int wic = nstl::max(SLC(), nstl::max(SIC(), DIC()));
        return (size_t)(L() + 1) * D() * (T() + 1) * S() * MB() * wic;
    }

    inline size_t ws_diff_states_size() {
        int wic = nstl::max(SLC(), nstl::max(SIC(), DIC()));
        return (size_t)(L() + 1) * D() * (T() + 1) * (S() + 1) * MB() * wic;
    }

    inline size_t ws_gates_size() {
        int n_layer = L();
        int n_direction = D();
        int n_iter = T();
        int n_gates = G();
        int batch = MB();
        int s_size = DIC();

        return (size_t)n_layer * n_direction * n_iter * batch * n_gates
                * s_size;
    }

    inline size_t ws_cell_comp_size() {
        int n_gates = G();
        int batch = MB();
        int s_size = DIC();
        return (size_t)is_lbr() * n_gates * batch * s_size;
    }

    inline size_t ws_grid_comp_size() {
        int n_layer = L();
        int n_direction = D();
        int n_iter = T();
        int batch = MB();
        int s_size = DIC();
        return (size_t)is_lbr() * is_training() * n_layer * n_direction * n_iter
                * batch * s_size;
    }

    inline int ws_per_cell() {
        int batch = MB();
        int s_size = DIC();
        return is_lbr() * is_training() * batch * s_size;
    }

    inline void set_offsets(size_t &ws_gates_offset, size_t &ws_states_offset,
            size_t &ws_diff_states_offset, size_t &ws_grid_comp_offset,
            size_t &ws_cell_comp_offset) {
        const size_t page_size = 4096; // 2097152;
        ws_gates_offset
                = 0; // assumes the workspace base pointer is page aligned
        ws_states_offset = utils::rnd_up(ws_gates_size(), page_size);
        ws_diff_states_offset
                = utils::rnd_up(ws_states_offset + ws_states_size(), page_size);
        ws_grid_comp_offset = utils::rnd_up(ws_diff_states_offset
                + ws_diff_states_size(), page_size);

        ws_cell_comp_offset = utils::rnd_up(ws_grid_comp_offset
                + ws_grid_comp_size(), page_size);
    }

    inline size_t get_ws_size() {
        size_t ws_gates_offset, ws_states_offset, ws_diff_states_offset,
            ws_grid_comp_offset, ws_cell_comp_offset;
        set_offsets(
                ws_gates_offset, ws_states_offset, ws_diff_states_offset,
                ws_grid_comp_offset, ws_cell_comp_offset);
        return ws_grid_comp_offset + ws_grid_comp_size();
    }

    inline size_t get_scratchpad_size() {
        size_t ws_gates_offset, ws_states_offset, ws_diff_states_offset,
            ws_grid_comp_offset, ws_cell_comp_offset;
        set_offsets(
                ws_gates_offset, ws_states_offset, ws_diff_states_offset,
                ws_grid_comp_offset, ws_cell_comp_offset);
        if (desc_.prop_kind == prop_kind::forward_inference)
            return ws_cell_comp_offset + ws_cell_comp_size();
        else
            return ws_cell_comp_size();
    }

    int T() const { return desc_.src_layer_desc.dims[0]; }
    int MB() const { return desc_.src_layer_desc.dims[1]; }

    int L() const { return desc_.weights_layer_desc.dims[0]; }
    int D() const { return desc_.weights_layer_desc.dims[1]; }

    int SIC() const { return desc_.weights_iter_desc.dims[2]; }

    int SLC() const { return desc_.weights_layer_desc.dims[2]; }
    int G() const { return desc_.weights_layer_desc.dims[3]; }
    int DIC() const { return desc_.weights_layer_desc.dims[4]; }

    int DLC() const { return desc_.dst_layer_desc.dims[2]; }

    int S() const { return mkldnn_rnn_cell_get_states_count(&desc_.cell_desc); }

    bool with_bias() const {
        return !memory_desc_wrapper(desc_.bias_desc).is_zero();
    }

    bool with_src_iter() const {
        return !(memory_desc_wrapper(desc_.src_iter_desc).is_zero());
    }

    bool with_dst_iter() const {
        return !memory_desc_wrapper(desc_.dst_iter_desc).is_zero();
    }

    mkldnn::impl::alg_kind_t cell_kind() const {
        return desc_.cell_desc.cell_kind;
    }
    mkldnn::impl::alg_kind_t activation_kind() const {
        return desc_.cell_desc.activation_kind;
    }

    bool is_lbr() const {
        return cell_kind() == mkldnn_gru_linear_before_reset;
    }

    mkldnn_rnn_direction_t direction() const { return desc_.direction; }

protected:
    rnn_desc_t desc_;
    const rnn_pd_t *hint_pd_;
};

struct rnn_fwd_pd_t : public rnn_pd_t {
    typedef rnn_fwd_pd_t base_class;
    typedef rnn_fwd_pd_t hint_class;

    using rnn_pd_t::rnn_pd_t;
    virtual ~rnn_fwd_pd_t() {}

    virtual const memory_pd_t *input_pd(int index = 0) const override {
        switch (index) {
        case 0: return src_pd(0);
        case 1: return src_pd(1);
        case 2: return weights_pd(0);
        case 3: return weights_pd(1);
        case 4: return weights_pd(2);
        default: return nullptr;
        }
    }

    virtual const memory_pd_t *output_pd(int index = 0) const override {
        switch (index) {
        case 0: return dst_pd(0);
        case 1: return dst_pd(1);
        case 2: return workspace_pd();
        default: return nullptr;
        }
    }

    virtual int n_inputs() const override {
        return 3 + with_bias() + with_src_iter();
    }

    virtual int n_outputs() const override {
        return 1 + with_dst_iter() + is_training();
    }

    int ws_idx() const { return 1 + with_dst_iter(); }
};

struct rnn_bwd_pd_t : public rnn_pd_t {
    typedef rnn_bwd_pd_t base_class;
    typedef rnn_bwd_pd_t hint_class;

    using rnn_pd_t::rnn_pd_t;
    virtual ~rnn_bwd_pd_t() {}

    virtual const memory_pd_t *input_pd(int index = 0) const override {
        switch (index) {
        case 0: return src_pd(0);
        case 1: return src_pd(1);
        case 2: return weights_pd(0);
        case 3: return weights_pd(1);
        case 4: return weights_pd(2);
        case 5: return dst_pd(0);
        case 6: return dst_pd(1);
        case 7: return diff_dst_pd(0);
        case 8: return diff_dst_pd(1);
        case 9: return workspace_pd();
        default: return nullptr;
        }
    }

    virtual const memory_pd_t *output_pd(int index = 0) const override {
        switch (index) {
        case 0: return diff_src_pd(0);
        case 1: return diff_src_pd(1);
        case 2: return diff_weights_pd(0);
        case 3: return diff_weights_pd(1);
        case 4: return diff_weights_pd(2);
        default: return nullptr;
        }
    }

    virtual int n_inputs() const override {
        return 6 + with_src_iter() + with_bias() + 2 * with_dst_iter();
    }
    virtual int n_outputs() const override {
        return 3 + with_src_iter() + with_bias();
    }

    int ws_idx() const {
        return 5 + with_src_iter() + with_bias() + 2 * with_dst_iter();
    }
};
}
}

#endif
