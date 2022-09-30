/*********************************************************************
 *
 * Copyright 2022 Collabora Ltd
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *********************************************************************/

#pragma once

#include "ft_frametrimmer.hpp"
#include "ft_dependecyobject.hpp"
#include "ft_matrixstate.hpp"

namespace frametrim {

enum eObjectType {
    ot_dxgi_object,
    ot_device,
    ot_swapchain,
    ot_device_child,
};

enum ePerDevice {
    pd_resources,
    pd_views,
    pd_class_linkages,
    pd_input_layouts,
    pd_shaders,
    pd_states,
    pd_asyncs,
};

enum ePerContextState {
    pc_blend,
    pc_depth_stencil,
    pc_rasterizer,
    pc_viewport,
    pc_scissor,
    pc_primitive_topology,
};

enum eShaderStage {
    ss_vertex,
    ss_hull,
    ss_domain,
    ss_geometry,
    ss_pixel,
    ss_compute,
};

enum ePerContextBinding {
    pcb_shaders,
    pcb_samplers,
    pcb_input_layout,
    pcb_vertex_buffers,
    pcb_index_buffer,
    pcb_constant_buffers,
    pcb_shader_resources,
    pcb_render_targets,
    pcb_depth_stencil_view,
    pcb_unordered_access_views,
    pcb_stream_out_targets,
};

enum eResourceType {
    rt_buffer,
    rt_texture_1d,
    rt_texture_2d,
    rt_texture_3d,
};

struct D3D11Device;

class D3D11DeviceChild: public UsedObject<void *> {
public:
    using Pointer = std::shared_ptr<D3D11DeviceChild>;

    D3D11DeviceChild(void *id);
};

using SubresourceId = std::pair<void *, unsigned>;
using ObjectBindings = std::unordered_map<unsigned, std::map<unsigned, D3D11DeviceChild::Pointer> >;

class D3D11Resource: public D3D11DeviceChild {
public:
    enum eResourceType m_kind;
    union {
        unsigned m_size;
        unsigned m_height;
        unsigned m_depth;
    };

    D3D11Resource(void *id, eResourceType kind, unsigned size);

    unsigned getMappedSize(unsigned width_stride, unsigned depth_stride);
};

class D3D11View: public D3D11DeviceChild {
public:
    D3D11View(void *id, D3D11Resource::Pointer res);

private:
    D3D11Resource::Pointer m_resource;
};

class D3D11Context: public D3D11DeviceChild {
public:
    using Pointer = std::shared_ptr<D3D11Context>;

    D3D11Context(void *id, std::shared_ptr<D3D11Device> device);

    ObjectBindings& getBindingsOfType(ePerContextBinding binding_type);

    std::map<unsigned, PTraceCall> m_state_calls;
    std::map<unsigned, D3D11DeviceChild::Pointer> m_state_deps;

    /* Bindings */
    ObjectBindings m_shaders;
    ObjectBindings m_samplers;
    ObjectBindings m_input_layout;
    ObjectBindings m_vertex_buffers;
    ObjectBindings m_index_buffer;
    ObjectBindings m_constant_buffers;
    ObjectBindings m_shader_resources;
    ObjectBindings m_render_targets;
    ObjectBindings m_depth_stencil_view;
    ObjectBindings m_unordered_access_views;
    ObjectBindings m_stream_out_targets;

private:
    std::weak_ptr<D3D11Device> m_device;
};

class DXGIObject: public UsedObject<void *> {
public:
    using Pointer = std::shared_ptr<DXGIObject>;

    DXGIObject(void *id);
};

class DXGISwapChain: public DXGIObject {
public:
    using Pointer = std::shared_ptr<DXGISwapChain>;

    DXGISwapChain(void *id, unsigned width, unsigned height);
    void resizeBuffers(unsigned width, unsigned height);
    unsigned getHeight() const;

private:
    unsigned m_width;
    unsigned m_height;
};

class D3D11Device: public UsedObject<void *>  {
public:
    using Pointer = std::shared_ptr<D3D11Device>;
    using ObjectPtr = D3D11DeviceChild::Pointer;

    D3D11Device(void *id);

    D3D11Context::Pointer m_context;
    // TODO deferred contexts
};

class D3D11Impl : public FrameTrimmer {
public:
    D3D11Impl(bool keep_all_states);

protected:
    void emitState();
    bool skipDeleteObj(const trace::Call& call);

private:
    void registerDeviceCalls();
    void registerContextCalls();

    void recordRequiredCall(const trace::Call& call);

    void updateCallTable(const std::vector<const char*>& names,
                           ft_callback cb);

    void releaseDXGIObject(const trace::Call& call);
    void releaseDevice(const trace::Call& call);
    void releaseDeviceChild(const trace::Call& call);
    void release(const trace::Call& call, eObjectType kind);

    void createDXGIFactory(const trace::Call& call);
    void enumAdapters(const trace::Call& call);
    void createSwapChain(const trace::Call& call);
    void resizeBuffers(const trace::Call& call);
    void resizeTarget(const trace::Call& call);
    void getBuffer(const trace::Call& call);
    void createDevice(const trace::Call& call);
    D3D11Device::Pointer getDevice(const trace::Call& call);
    D3D11Context::Pointer getContext(const trace::Call& call);
    void getImmediateContext(const trace::Call& call);

    void addDependencies(D3D11DeviceChild::Pointer child, const trace::Array *deps);
    void create(const trace::Call& call, ePerDevice object_type, unsigned obj_id_param);
    void createWithDep(const trace::Call& call, ePerDevice object_type, unsigned obj_id_param,
                       ePerDevice dep_type, unsigned dep_id_param);
    void bindObject(const trace::Call& call, ePerContextBinding binding_type,
                    unsigned bindpoint, unsigned slot, void *bound_obj_id);
    void bindObjects(const trace::Call& call, ePerContextBinding binding_type, unsigned bindpoint,
                     unsigned slot, unsigned ids_param);
    void bindSlot(const trace::Call& call, ePerContextBinding binding_type, unsigned id_param);
    void bindSlots(const trace::Call& call, ePerContextBinding binding_type, unsigned bindpoint);
    void callOnObject(const trace::Call& call, ePerDevice map_type, unsigned id_param);
    void callOnObjectWithDep(const trace::Call& call, ePerDevice map_type, unsigned id_param,
                             ePerDevice dep_type, unsigned dep_id_param);

    void createState(const trace::Call& call);
    void setState(const trace::Call& call, ePerContextState state_flag);
    void bindState(const trace::Call& call, ePerContextState state_flag);
    void clearState(const trace::Call& call);

    void createShader(const trace::Call& call);
    void createShaderWithStreamOutput(const trace::Call& call);
    void bindShader(const trace::Call& call, unsigned bindpoint);

    void createResource(const trace::Call& call, eResourceType kind, unsigned size);
    void createBuffer(const trace::Call& call);
    void createTexture1D(const trace::Call& call);
    void createTexture2D(const trace::Call& call);
    void createTexture3D(const trace::Call& call);

    void createView(const trace::Call& call);
    void clearView(const trace::Call& call);
    void bindRenderTargets(const trace::Call& call);
    void bindRenderTargetsAndUAVS(const trace::Call& call);

    void createAsync(const trace::Call& call);

    void memcopy(const trace::Call& call);
    void map(const trace::Call& call);
    void unmap(const trace::Call& call);
    void copyResource(const trace::Call& call, unsigned dst_param_id, unsigned src_param_id);
    void draw(const trace::Call& call);

    std::unordered_map<void *, DXGIObject::Pointer> m_dxgi_objects;
    std::unordered_map<void *, D3D11Device::Pointer> m_devices;
    std::unordered_map<void *, D3D11DeviceChild::Pointer> m_children;
    std::map<SubresourceId, std::pair<uintptr_t, uintptr_t>> m_buffer_mappings;
    std::map<unsigned, PTraceCall> m_state_calls;
    std::map<unsigned, PTraceCall> m_enables;
};

}
