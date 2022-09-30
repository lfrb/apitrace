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

#include "ft_d3d11.hpp"
#include "ft_tracecall.hpp"
#include "ft_matrixstate.hpp"
#include "ft_dependecyobject.hpp"

#include "trace_model.hpp"

#include <unordered_set>
#include <algorithm>
#include <functional>
#include <sstream>
#include <iostream>
#include <memory>
#include <set>

namespace frametrim {

using std::bind;
using std::placeholders::_1;
using std::make_shared;

DXGIObject::DXGIObject(void *id):
    UsedObject<void *>(id)
{
}

DXGISwapChain::DXGISwapChain(void *id, unsigned width, unsigned height):
    DXGIObject(id),
    m_width(width),
    m_height(height)
{
}

void
DXGISwapChain::resizeBuffers(unsigned width, unsigned height)
{
    m_width = width;
    m_height = height;
}

unsigned
DXGISwapChain::getHeight() const
{
    return m_height;
}

D3D11DeviceChild::D3D11DeviceChild(void *id):
    UsedObject<void *>(id)
{
}

D3D11Resource::D3D11Resource(void *id, eResourceType kind, unsigned size):
    D3D11DeviceChild(id),
    m_kind(kind),
    m_size(size)
{
}

unsigned
D3D11Resource::getMappedSize(unsigned width_stride, unsigned depth_stride)
{
    switch (m_kind) {
    case rt_buffer: return m_size;
    case rt_texture_1d: return width_stride;
    case rt_texture_2d: return m_height * width_stride;
    case rt_texture_3d: return m_depth * depth_stride;
    default: assert(0);
    }
}

D3D11View::D3D11View(void *id, D3D11Resource::Pointer res):
    D3D11DeviceChild(id),
    m_resource(res)
{
}

D3D11Context::D3D11Context(void *id, std::shared_ptr<D3D11Device> device):
    D3D11DeviceChild(id),
    m_device(device)
{
}



ObjectBindings&
D3D11Context::getBindingsOfType(ePerContextBinding binding_type)
{
    switch (binding_type) {
    case pcb_shaders: return m_shaders;
    case pcb_samplers: return m_samplers;
    case pcb_input_layout: return m_input_layout;
    case pcb_vertex_buffers: return m_vertex_buffers;
    case pcb_index_buffer: return m_index_buffer;
    case pcb_constant_buffers: return m_constant_buffers;
    case pcb_shader_resources: return m_shader_resources;
    case pcb_render_targets: return m_render_targets;
    case pcb_depth_stencil_view: return m_depth_stencil_view;
    case pcb_unordered_access_views: return m_unordered_access_views;
    case pcb_stream_out_targets: return m_stream_out_targets;
    default: assert(0);
    }
}

D3D11Device::D3D11Device(void *id):
    UsedObject<void *>(id)
{
}

D3D11Impl::D3D11Impl(bool keep_all_states):
    FrameTrimmer(keep_all_states)
{
    registerDeviceCalls();
    registerContextCalls();
}

void D3D11Impl::emitState()
{
}

// Map callbacks to call methods of FrameTrimImpl
// Additional data is passed by reference (R) or by value (V)

#define MAP(name, call) m_call_table.insert(std::make_pair(#name, bind(&D3D11Impl:: call, this, _1)))

#define MAP_V(name, call, ...) \
    m_call_table.insert(std::make_pair(#name, bind(&D3D11Impl:: call, this, _1, ## __VA_ARGS__)))

#define MAP_FACTORY(name, call) \
    m_call_table.insert(std::make_pair("IDXGIFactory1::" #name, bind(&D3D11Impl:: call, this, _1))) \

#define MAP_DEV(name, call) \
    m_call_table.insert(std::make_pair("ID3D11Device::" #name, bind(&D3D11Impl:: call, this, _1)))

#define MAP_DEV_V(name, call, ...) \
    m_call_table.insert(std::make_pair("ID3D11Device::" #name, bind(&D3D11Impl:: call, this, _1, \
                                                                    ## __VA_ARGS__)))

#if 0

void D3D11Impl::registerIgnoredCalls()
{
/*
    "*::GetDesc",
    "*::GetDesc1"
*/
}

#endif


void D3D11Impl::registerDeviceCalls()
{
    MAP_V(IDXGIAdapter::Release, releaseDXGIObject);
    MAP_V(IDXGIFactory::Release, releaseDXGIObject);
    MAP_V(IDXGIOutput::Release, releaseDXGIObject);
    MAP_V(ID3D11Device::Release, releaseDevice);
    MAP_V(ID3D11Buffer::Release,releaseDeviceChild);
    MAP_V(ID3D11Texture1D::Release,releaseDeviceChild);
    MAP_V(ID3D11Texture2D::Release,releaseDeviceChild);
    MAP_V(ID3D11Texture3D::Release,releaseDeviceChild);
    MAP_V(ID3D11DomainShader::Release,releaseDeviceChild);
    MAP_V(ID3D11FragmentShader::Release,releaseDeviceChild);
    MAP_V(ID3D11GeometryShader::Release,releaseDeviceChild);
    MAP_V(ID3D11HullShader::Release,releaseDeviceChild);
    MAP_V(ID3D11HullShader::Release,releaseDeviceChild);
    MAP_V(ID3D11PixelShader::Release,releaseDeviceChild);

    MAP(CreateDXGIFactory, createDXGIFactory);
    MAP(D3D11CreateDevice, createDevice);
    MAP(D3D11CreateDeviceAndSwapChain, createDevice);
    MAP(IDXGIFactory::CreateSwapChainForHwnd, createSwapChain);
    MAP(IDXGIFactory::EnumAdapters, enumAdapters);
    MAP(IDXGISwapChain::ResizeBuffers, resizeBuffers);
    MAP(IDXGISwapChain::ResizeTarget, resizeTarget);
    MAP(IDXGISwapChain::GetBuffer, getBuffer);
    MAP_DEV(GetImmediateContext, getImmediateContext);
    MAP_DEV(GetImmediateContext1, getImmediateContext);

    MAP_DEV(CreateBlendState, createState);
    MAP_DEV(CreateDepthStencilState, createState);
    MAP_DEV(CreateRasterizerState, createState);
    MAP_DEV(CreateSamplerState, createState);

    MAP_DEV_V(CreateClassLinkage, create, pd_class_linkages, 1);
    MAP_DEV_V(CreateInputLayout, create, pd_input_layouts, 5);

    MAP_DEV(CreateVertexShader, createShader);
    MAP_DEV(CreateHullShader, createShader);
    MAP_DEV(CreateDomainShader, createShader);
    MAP_DEV(CreateGeometryShader, createShader);
    MAP_DEV(CreateGeometryShaderWithStreamOutput, createShaderWithStreamOutput);
    MAP_DEV(CreatePixelShader, createShader);
    MAP_DEV(CreateComputeShader, createShader);

    MAP_DEV(CreateBuffer, createBuffer);
    MAP_DEV(CreateTexture1D, createTexture1D);
    MAP_DEV(CreateTexture2D, createTexture2D);
    MAP_DEV(CreateTexture3D, createTexture3D);

    MAP_DEV(CreateShaderResourceView, createView);
    MAP_DEV(CreateUnorderedAccessView, createView);
    MAP_DEV(CreateRenderTargetView, createView);
    MAP_DEV(CreateDepthStencilView, createView);

    MAP_DEV(CreateQuery, createAsync);
    MAP_DEV(CreateCounter, createAsync);
    MAP_DEV(CreatePredicate, createAsync);
}

#define MAP_CTX(name, call) \
    m_call_table.insert(std::make_pair("ID3D11DeviceContext::" #name, bind(&D3D11Impl:: call, this, _1)))

#define MAP_CTX_V(name, call, ...) \
    m_call_table.insert(std::make_pair("ID3D11DeviceContext::" #name, bind(&D3D11Impl:: call, this, _1, ## __VA_ARGS__)))

void
D3D11Impl::registerContextCalls()
{
    MAP_CTX(ClearState, clearState);
    //TODO MAP_CTX(Flush, addCall);

    MAP_CTX_V(IASetIndexBuffer, bindSlot, pcb_index_buffer, 1);
    MAP_CTX_V(IASetVertexBuffers, bindSlots, pcb_vertex_buffers, ss_vertex);
    MAP_CTX_V(IASetInputLayout, bindSlot, pcb_input_layout, 1);
    MAP_CTX_V(IASetPrimitiveTopology, setState, pc_primitive_topology);

    MAP_CTX_V(OMSetBlendState, bindState, pc_blend);
    MAP_CTX_V(OMSetDepthStencilState, bindState, pc_depth_stencil);
    MAP_CTX(OMSetRenderTargets, bindRenderTargets);
    MAP_CTX(OMSetRenderTargetsAndUnorderedAccessViews, bindRenderTargetsAndUAVS);

    MAP_CTX_V(RSSetViewports, setState, pc_viewport);
    MAP_CTX_V(RSSetScissorRects, setState, pc_scissor);
    MAP_CTX_V(RSSetState, bindState, pc_rasterizer);

    MAP_CTX_V(SOSetTargets, bindObjects, pcb_stream_out_targets, 0, 0, 2);

    MAP_CTX_V(VSSetShader, bindShader, ss_vertex);
    MAP_CTX_V(HSSetShader, bindShader, ss_hull);
    MAP_CTX_V(DSSetShader, bindShader, ss_domain);
    MAP_CTX_V(GSSetShader, bindShader, ss_geometry);
    MAP_CTX_V(PSSetShader, bindShader, ss_pixel);
    MAP_CTX_V(CSSetShader, bindShader, ss_compute);

    MAP_CTX_V(VSSetSamplers, bindSlots, pcb_samplers, ss_vertex);
    MAP_CTX_V(HSSetSamplers, bindSlots, pcb_samplers, ss_hull);
    MAP_CTX_V(DSSetSamplers, bindSlots, pcb_samplers, ss_domain);
    MAP_CTX_V(GSSetSamplers, bindSlots, pcb_samplers, ss_geometry);
    MAP_CTX_V(PSSetSamplers, bindSlots, pcb_samplers, ss_pixel);
    MAP_CTX_V(CSSetSamplers, bindSlots, pcb_samplers, ss_compute);

    MAP_CTX_V(VSSetShaderResources, bindSlots, pcb_shader_resources, ss_vertex);
    MAP_CTX_V(HSSetShaderResources, bindSlots, pcb_shader_resources, ss_hull);
    MAP_CTX_V(DSSetShaderResources, bindSlots, pcb_shader_resources, ss_domain);
    MAP_CTX_V(GSSetShaderResources, bindSlots, pcb_shader_resources, ss_geometry);
    MAP_CTX_V(PSSetShaderResources, bindSlots, pcb_shader_resources, ss_pixel);
    MAP_CTX_V(CSSetShaderResources, bindSlots, pcb_shader_resources, ss_compute);

    MAP_CTX_V(VSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_vertex);
    MAP_CTX_V(HSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_hull);
    MAP_CTX_V(DSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_domain);
    MAP_CTX_V(GSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_geometry);
    MAP_CTX_V(PSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_pixel);
    MAP_CTX_V(CSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_compute);

    MAP_CTX(ClearView, clearView);
    MAP_CTX(ClearRenderTargetView, clearView);
    MAP_CTX(ClearDepthStencilView, clearView);
    MAP_CTX(ClearUnorderedAccessViewFloat, clearView);
    MAP_CTX(ClearUnorderedAccessViewUint, clearView);

    MAP(memcpy, memcopy);
    MAP_CTX(Map, map);
    MAP_CTX(Unmap, unmap);
    MAP_CTX_V(UpdateResource, callOnObject, pd_resources, 1);
    MAP_CTX_V(UpdateSubresource, callOnObject, pd_resources, 1);
    MAP_CTX_V(CopyResources, copyResource, 1, 2);
    MAP_CTX_V(CopyStructureCount, copyResource, 1, 3);
    MAP_CTX_V(CopySubresourceRegion, copyResource, 1, 6);
    MAP_CTX_V(GenerateMips, callOnObject, pd_views, 1);

#if 0
    MAP_CTX(SetPredication);
    MAP_CTX(Begin); // query, predicate and counter
    MAP_CTX(End);
    MAP_CTX(GetData);
#endif

    MAP_CTX(Draw, draw);
    MAP_CTX(DrawAuto, draw);
    MAP_CTX(DrawIndexed, draw);
    MAP_CTX(DrawIndexedInstanced, draw);
    MAP_CTX(DrawIndexedInstancedIndirect, draw); // XXX buffer binding
    MAP_CTX(DrawInstanced, draw);
    MAP_CTX(DrawInstancedIndirect, draw); // XXX buffer binding
}

bool D3D11Impl::skipDeleteObj(const trace::Call& call)
{
    return false;
}

#define IS_INTERFACE(call, interface) (strncmp(call.name(), #interface, strlen(#interface)) == 0)

void D3D11Impl::release(const trace::Call& call, eObjectType kind)
{
    if (call.ret->toUInt() != 0)
        return;

    auto id = call.arg(0).toPointer();
    //std::cout << "release " << id << " ret = " << call.ret->toUInt() << "\n";
    if (IS_INTERFACE(call, "IDXGIAdapter"))
        std::cout << "release adapter " << id << "\n";
}

void D3D11Impl::releaseDXGIObject(const trace::Call& call)
{
    if (call.ret->toUInt() != 0)
        return;

    auto id = call.arg(0).toPointer();
    //std::cout << "release " << id << " ret = " << call.ret->toUInt() << "\n";
    m_dxgi_objects.erase(id);
}

void D3D11Impl::releaseDevice(const trace::Call& call)
{
    if (call.ret->toUInt() != 0)
        return;

    auto id = call.arg(0).toPointer();
    //std::cout << "release " << id << " ret = " << call.ret->toUInt() << "\n";
    m_devices.erase(id);
}

void D3D11Impl::releaseDeviceChild(const trace::Call& call)
{
    if (call.ret->toUInt() != 0)
        return;

    auto id = call.arg(0).toPointer();
    //std::cout << "release " << id << " ret = " << call.ret->toUInt() << "\n";
    m_children.erase(id);
}

static void *
unwrapObj(const trace::Call& call, const char *name)
{
    auto array = call.argByName(name).toArray();
    return array->values[0]->toPointer();
}

static void *
unwrapObjAt(const trace::Call& call, unsigned index)
{
    auto array = call.arg(index).toArray();
    return array->values[0]->toPointer();
}

void D3D11Impl::createDXGIFactory(const trace::Call& call)
{
    auto id = unwrapObj(call, "ppFactory");
    m_dxgi_objects[id] = std::make_shared<DXGIObject>(id);
    m_dxgi_objects[id]->addCall(trace2call(call));
}

void D3D11Impl::createDevice(const trace::Call& call)
{
    auto adapter = m_dxgi_objects[call.arg(0).toPointer()];
    auto id = unwrapObj(call, "ppDevice");
    m_devices[id] = std::make_shared<D3D11Device>(id);
    std::cout << "createDevice " << m_devices[id] << " for adapter " << call.arg(0).toPointer() << " (" << adapter << ")\n";
    m_devices[id]->addCall(trace2call(call));
    m_devices[id]->addDependency(adapter);
    getImmediateContext(call);
}

void D3D11Impl::getImmediateContext(const trace::Call& call)
{
    auto id = unwrapObj(call, "ppDevice"); // XXX
    // TODO check whether the device exists
    if (!m_devices[id]->m_context) {
        auto ctx_id = unwrapObj(call, "ppImmediateContext");
        auto ctx = std::make_shared<D3D11Context>(ctx_id, m_devices[id]);
        m_devices[id]->m_context = ctx;
        m_children[ctx_id] = ctx;
    }
    m_devices[id]->addCall(trace2call(call));
}

void D3D11Impl::createSwapChain(const trace::Call& call)
{
    auto id = unwrapObj(call, "ppSwapChain");
    auto device = m_devices[call.arg(1).toPointer()];
    auto desc = call.arg(3).toArray()->values[0]->toStruct();
    auto width = desc->members[0]->toUInt();
    auto height = desc->members[1]->toUInt();

    m_dxgi_objects[id] = std::make_shared<DXGISwapChain>(id, width, height);
    m_dxgi_objects[id]->addCall(trace2call(call));
    m_dxgi_objects[id]->addDependency(device);
}

void D3D11Impl::enumAdapters(const trace::Call& call)
{
    if (call.ret->toSInt() != 0)
        return;

    auto factory = m_dxgi_objects[call.arg(0).toPointer()];
    auto id = unwrapObj(call, "ppAdapter");

    if (!m_dxgi_objects[id]) {
        auto adapter = std::make_shared<DXGIObject>(id);
        adapter->addCall(trace2call(call));
        adapter->addDependency(factory);
        m_dxgi_objects[id] = adapter;
        std::cout << "enum adapter " << call.arg(1).toUInt() << " is " << id << " (" << adapter << ")\n";
    }
}

void D3D11Impl::resizeTarget(const trace::Call& call)
{
    auto swapchain = m_dxgi_objects[call.arg(0).toPointer()];
    swapchain->addCall(trace2call(call));
    if (m_recording_frame && swapchain->emitted())
        swapchain->emitCallsTo(m_required_calls);
}

void D3D11Impl::resizeBuffers(const trace::Call& call)
{
    auto swapchain = std::static_pointer_cast<DXGISwapChain>(m_dxgi_objects[call.arg(0).toPointer()]);
    swapchain->resizeBuffers(call.arg(2).toUInt(), call.arg(3).toUInt());
    swapchain->addCall(trace2call(call));
    if (m_recording_frame && swapchain->emitted())
        swapchain->emitCallsTo(m_required_calls);
}

void D3D11Impl::getBuffer(const trace::Call& call)
{
    auto swapchain = std::static_pointer_cast<DXGISwapChain>(m_dxgi_objects[call.arg(0).toPointer()]);
    auto id = unwrapObj(call, "ppSurface"); 
    
    if (!m_children[id]) {
        m_children[id] = std::make_shared<D3D11Resource>(id, rt_texture_2d, swapchain->getHeight());
        m_children[id]->addCall(trace2call(call)); // FIXME
        m_children[id]->addDependency(swapchain);
    }
}

D3D11Device::Pointer D3D11Impl::getDevice(const trace::Call& call)
{
    auto device = m_devices[call.arg(0).toPointer()];
    assert(device);
    return device;
}

D3D11Context::Pointer D3D11Impl::getContext(const trace::Call& call)
{
    auto context = m_children[call.arg(0).toPointer()];
    assert(context);
    return std::static_pointer_cast<D3D11Context>(context);
}

void D3D11Impl::addDependencies(D3D11DeviceChild::Pointer child, const trace::Array *deps)
{
    for (auto dep: deps->values) {
        child->addDependency(m_children[dep->toPointer()]);
    }
}

void D3D11Impl::create(const trace::Call& call, ePerDevice object_type, unsigned obj_id_param)
{
    auto id = unwrapObjAt(call, obj_id_param);
    m_children[id] = std::make_shared<D3D11DeviceChild>(id);
    m_children[id]->addCall(trace2call(call));
}

void D3D11Impl::createWithDep(const trace::Call& call, ePerDevice obj_type, unsigned obj_id_param,
                              ePerDevice dep_type, unsigned dep_id_param)
{
    auto id = unwrapObjAt(call, obj_id_param);
    auto dep_id = call.arg(dep_id_param).toPointer();

    m_children[id] = std::make_shared<D3D11DeviceChild>(id);
    if (dep_id) {
        if (m_children[dep_id])
            m_children[id]->addDependency(m_children[dep_id]);
        else
            std::cout << "error no dep " << dep_id <<  "\n";
    }
    m_children[id]->addCall(trace2call(call));
}

void
D3D11Impl::bindObject(const trace::Call& call, ePerContextBinding binding_type,
                      unsigned bindpoint, unsigned slot, void *bound_obj_id)
{
    auto ctx = getContext(call);
    auto bindings = ctx->getBindingsOfType(binding_type);
    auto bound_obj = bound_obj_id ? m_children[bound_obj_id] : nullptr;

    bindings[bindpoint][slot] = bound_obj;
    if (bound_obj)
        bound_obj->addCall(trace2call(call));
    else
        ctx->addCall(trace2call(call));

    if (m_recording_frame && bound_obj)
        bound_obj->emitCallsTo(m_required_calls);
}

void
D3D11Impl::bindObjects(const trace::Call& call, ePerContextBinding binding_type,
                       unsigned bindpoint, unsigned slot, unsigned param_id)
{
    auto objects = call.arg(param_id).toArray();

    // TODO clear all current bindings
    if (!objects)
        return;

    for (auto object: objects->values) {
        bindObject(call, binding_type, bindpoint, slot, object->toPointer());
        slot++;
    }
}

void
D3D11Impl::bindSlot(const trace::Call& call, ePerContextBinding binding_type, unsigned param_id)
{
    auto res_id = call.arg(param_id).toPointer();
    bindObject(call, binding_type, 0, 0, res_id);
}

void
D3D11Impl::bindSlots(const trace::Call& call, ePerContextBinding binding_type, unsigned bindpoint)
{
    auto slot = call.arg(1).toUInt();
    bindObjects(call, binding_type, bindpoint, slot, 3);
}

void
D3D11Impl::callOnObject(const trace::Call& call, ePerDevice obj_type, unsigned obj_id_param)
{
    auto id = call.arg(obj_id_param).toPointer();
    auto obj = m_children[id];
    obj->addCall(trace2call(call));
    // XXX what about view <-> resource
}

void
D3D11Impl::callOnObjectWithDep(const trace::Call& call, ePerDevice obj_type, unsigned obj_id_param,
                               ePerDevice dep_type, unsigned dep_id_param)
{
    auto id = call.arg(obj_id_param).toPointer();
    auto dep_id = call.arg(dep_id_param).toPointer();
    auto obj = m_children[id];

    obj->addCall(trace2call(call));
    if (!m_children[dep_id]) {
        std::cout << "callOnObjectWithDep with null dep\n";
        return;
    }
    obj->addDependency(m_children[dep_id]);
    // XXX what about view <-> resource
}

void D3D11Impl::createState(const trace::Call& call)
{
    create(call, pd_states, 2);
}

void D3D11Impl::createShader(const trace::Call& call)
{
    createWithDep(call, pd_shaders, 4, pd_class_linkages, 3);
}

void D3D11Impl::createShaderWithStreamOutput(const trace::Call& call)
{
    createWithDep(call, pd_shaders, 9, pd_class_linkages, 8);
}

void D3D11Impl::bindShader(const trace::Call& call, unsigned bindpoint)
{
    auto ctx = getContext(call);

    auto shader_id = call.arg(1).toPointer();
    //XXX auto linkages = call.arg(2).toArray();
    //addDependencies(ctx, linkages);

    bindObject(call, pcb_shaders, bindpoint, 0, shader_id);
}

void D3D11Impl::createResource(const trace::Call& call, eResourceType kind, unsigned size)
{
    auto id = unwrapObjAt(call, 3);
    m_children[id] = std::make_shared<D3D11Resource>(id, kind, size);
    m_children[id]->addCall(trace2call(call));
}

void D3D11Impl::createBuffer(const trace::Call& call)
{
    /* D3D11_BUFFER_DESC.ByteWidth */
    unsigned size = call.arg(1).toArray()->values[0]->toStruct()->members[0]->toUInt();
    createResource(call, rt_buffer, size);
}

void D3D11Impl::createTexture1D(const trace::Call& call)
{
    /* Use byte size from map call */
    createResource(call, rt_texture_1d, 1);
}

void D3D11Impl::createTexture2D(const trace::Call& call)
{
    /* D3D11_TEXTURE2D_DESC.Height */
    unsigned height = call.arg(1).toArray()->values[0]->toStruct()->members[1]->toUInt();
    createResource(call, rt_texture_2d, height);
}

void D3D11Impl::createTexture3D(const trace::Call& call)
{
    /* D3D11_TEXTURE3D_DESC.Depth */
    unsigned depth = call.arg(1).toArray()->values[0]->toStruct()->members[2]->toUInt();
    createResource(call, rt_texture_3d, depth);
}

void D3D11Impl::createView(const trace::Call& call)
{
    auto id = unwrapObjAt(call, 3);
    auto dep_id = call.arg(1).toPointer();
    auto res = std::static_pointer_cast<D3D11Resource>(m_children[dep_id]);

    m_children[id] = std::make_shared<D3D11View>(id, res);
    if (!m_children[dep_id]) {
        std::cout << "resource " << dep_id << " for new view doesn't exist\n";
        return;
    }
    m_children[id]->addDependency(m_children[dep_id]);
    m_children[dep_id]->addDependency(m_children[id]);
    m_children[id]->addCall(trace2call(call));
}

void D3D11Impl::bindRenderTargets(const trace::Call& call)
{
    bindObjects(call, pcb_render_targets, 0, 0, 2);
    bindSlot(call, pcb_depth_stencil_view, 3);
}

void D3D11Impl::bindRenderTargetsAndUAVS(const trace::Call& call)
{
    bindObjects(call, pcb_render_targets, 0, 0, 2);
    bindSlot(call, pcb_depth_stencil_view, 3);
    bindObjects(call, pcb_unordered_access_views, 0, 0, 5);
}

void D3D11Impl::clearView(const trace::Call& call)
{
    auto view = std::static_pointer_cast<D3D11View>(m_children[call.arg(1).toPointer()]);

    //XXX view->emitCallsTo(view->resource);
    view->addCall(trace2call(call));
}

void D3D11Impl::createAsync(const trace::Call& call)
{
    create(call, pd_asyncs, 2);
}

void D3D11Impl::setState(const trace::Call& call, ePerContextState state_flag)
{
    auto ctx = getContext(call);
    ctx->m_state_calls[state_flag] = trace2call(call);

    // TODO if deferred context
}

void D3D11Impl::bindState(const trace::Call& call, ePerContextState state_flag)
{
    auto ctx = getContext(call);
    auto dep_id = call.arg(1).toPointer();

    if (dep_id) {
        ctx->m_state_calls[state_flag] = trace2call(call);
        ctx->m_state_deps[state_flag] = m_children[dep_id];

        if (m_recording_frame) {
            m_children[dep_id]->emitCallsTo(m_required_calls);
        }
    } else {
        ctx->m_state_calls.erase(state_flag);
        ctx->m_state_deps.erase(state_flag);
    }
}

void D3D11Impl::clearState(const trace::Call& call)
{
    auto ctx = getContext(call);

    ctx->m_state_calls.clear();
    ctx->m_state_deps.clear();
    ctx->m_render_targets.clear();
    // TODO
}

void D3D11Impl::memcopy(const trace::Call& call)
{
    uintptr_t start = call.arg(0).toUInt();
    uintptr_t data_end = start + call.arg(2).toUInt();
    SubresourceId res_id = std::make_pair(nullptr, 0);

    for (auto&& [id, range ]: m_buffer_mappings) {
        if (range.first <= start && start < range.second) {
            if (data_end > range.second) {
                std::cerr << "\n:Error "<< call.no << "(memcpy): Mapped target range is ["
                          << range.first << ", " << range.second << "] but data requires ["
                          << start << ", " << data_end << "]\n";
                assert(0);
            }

            res_id = id;
            break;
        }
    }
    if (!res_id.first) {
        std::cerr << "Found no mapping for memcopy to " << start << " in call " << call.no << ": " << call.name() << "\n";
        assert(0);
        return;
    }

    auto res = m_children[res_id.first];
    res->addCall(trace2call(call));

    if (m_recording_frame)
        res->emitCallsTo(m_required_calls);
}

void D3D11Impl::map(const trace::Call& call)
{
    auto ctx = getContext(call);
    auto res = std::static_pointer_cast<D3D11Resource>(m_children[call.arg(1).toPointer()]);
    auto subres = call.arg(2).toUInt();
    auto mapped_subres = call.arg(5).toArray()->values[0]->toStruct();
    uintptr_t begin = reinterpret_cast<uintptr_t>(mapped_subres->members[0]->toPointer());
    auto row_pitch = mapped_subres->members[1]->toUInt();
    auto depth_pitch = mapped_subres->members[2]->toUInt();
    auto end = begin + res->getMappedSize(row_pitch, depth_pitch);

    m_buffer_mappings[std::make_pair(res->id(), subres)] = std::make_pair(begin, end);
    res->addCall(trace2call(call));

    // TODO https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map#null-pointers-for-pmappedresource
    /*auto map_type = call.arg(3).toUInt();
    if (map_type == D3D11_MAP_WRITE_DISCARD) {
        // XXX only keep resource creation call
    }*/
}

void D3D11Impl::unmap(const trace::Call& call)
{
    auto res = std::static_pointer_cast<D3D11Resource>(m_children[call.arg(1).toPointer()]);
    auto subres = call.arg(2).toUInt();
    auto id = std::make_pair(res->id(), subres);

    m_buffer_mappings.erase(id);
    res->addCall(trace2call(call));
}

void D3D11Impl::copyResource(const trace::Call& call, unsigned dst_param_id, unsigned src_param_id)
{
    callOnObjectWithDep(call, pd_resources, dst_param_id, pd_resources, src_param_id);
}

void addBoundAsDependencyTo(ObjectBindings& bindings, D3D11DeviceChild::Pointer obj)
{
    for (auto [bindpoint, slots]: bindings) {
        for (auto [slot, dep]: slots) {
            if (!dep) {
                std::cout << "null bound dep\n";
                continue;
            }
            obj->addDependency(dep);
        }
    }
}

void addDependencyToBound(ObjectBindings& bindings, D3D11DeviceChild::Pointer dep)
{
            if (!dep) {
                std::cout << "null dep\n";
                return;
            }
    for (auto [bindpoint, slots]: bindings) {
        for (auto [slot, obj]: slots) {
            obj->addDependency(dep);
        }
    }
}

void D3D11Impl::draw(const trace::Call& call)
{
    auto ctx = getContext(call);
    auto draw = make_shared<D3D11DeviceChild>((void *)call.no);

    for (auto [state_flag, call]: ctx->m_state_calls) {
        draw->addCall(call);
    }
    for (auto [state_flag, dep]: ctx->m_state_deps) {
        draw->addDependency(dep);
    }

    // XXX add current state as dependency

    addBoundAsDependencyTo(ctx->m_shaders, draw);
    addBoundAsDependencyTo(ctx->m_samplers, draw);
    addBoundAsDependencyTo(ctx->m_input_layout, draw);
    addBoundAsDependencyTo(ctx->m_shader_resources, draw);
    addBoundAsDependencyTo(ctx->m_constant_buffers, draw);
    addBoundAsDependencyTo(ctx->m_vertex_buffers, draw);
    addBoundAsDependencyTo(ctx->m_index_buffer, draw);
    addBoundAsDependencyTo(ctx->m_render_targets, draw);

    addDependencyToBound(ctx->m_render_targets, draw);
    addDependencyToBound(ctx->m_depth_stencil_view, draw);
    addDependencyToBound(ctx->m_stream_out_targets, draw);

    draw->addCall(trace2call(call));
    if (m_recording_frame) {
        std::cout << "recording frame\n";
        draw->emitCallsTo(m_required_calls);
    }
}

#if 0
void
D3D11Impl::updateCallTable(const std::vector<const char*>& names,
                                        ft_callback cb)
{
    for (auto& i : names)
        m_call_table.insert(std::make_pair(i, cb));
}

void
D3D11Impl::recordRequiredCall(const trace::Call& call)
{
    auto c = trace2call(call);
    m_required_calls.insert(c);
}
#endif

}
