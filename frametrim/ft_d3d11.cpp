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

static D3D11Box *
toBox(const trace::Call& call, unsigned index)
{
    auto array = call.arg(index).toArray();
    if (!array)
        return nullptr;

    auto box = array->values[0]->toStruct();
    auto left = box->members[0]->toUInt();
    auto top = box->members[1]->toUInt();
    auto front = box->members[2]->toUInt();
    auto right = box->members[3]->toUInt();
    auto bottom = box->members[4]->toUInt();
    auto back = box->members[5]->toUInt();

    return new D3D11Box(left, top, front, right, bottom, back);
}

D3D11Box::D3D11Box(unsigned left, unsigned top, unsigned front, unsigned right, unsigned bottom, unsigned back):
    left(left),
    top(top),
    front(front),
    right(right),
    bottom(bottom),
    back(back)
{
}

bool
D3D11Box::isEmpty() const
{
    return (right <= left || bottom <= top || back <= front);
}

D3D11Mapping::D3D11Mapping(const trace::Call& call, unsigned subres,
                           unsigned long long start, unsigned long long end,
                           bool discard):
    subres(subres),
    start(start),
    end(end),
    range_min(UINT_MAX),
    range_max(0),
    discard(discard)
{
    calls.push_back(trace2call(call));
}

void
D3D11Mapping::update(const trace::Call& call, unsigned long long update_start, unsigned long long update_end)
{
    calls.push_back(trace2call(call));

    if (!discard) {
        unsigned min = update_start - start;
        unsigned max = update_end - start;
        range_min = std::min(range_min, min);
        range_max = std::max(range_max, max);
    }
}

void
D3D11Mapping::finish(const trace::Call& call)
{
    calls.push_back(trace2call(call));
}

Interface::Interface(const char *name, Pointer parent):
    m_name(name),
    m_parent(parent)
{
}

Interface::Interface(const char *name):
    m_name(name),
    m_parent(std::shared_ptr<Interface>())
{
}

void Interface::addCall(const char *name, ft_callback callback)
{
    m_call_table.insert({name, callback});
}

ft_callback Interface::findCall(const char *name)
{
    auto iter = m_call_table.find(name);
    if (iter != m_call_table.end())
        return iter->second;
    if (m_parent)
        return m_parent->findCall(name);
    return nullptr;
}

Object::Object(ImplPtr impl, void *pointer):
    m_impl(impl),
    m_id(pointer),
    m_refcount(1),
    m_emitted(false),
    m_emitting(false),
    m_unrolled(false)
{
}

PTraceCall Object::getInitCall() const
{
    return m_init_call;
}

void Object::setInitCall(const trace::Call& call)
{
    m_init_call = trace2call(call);
}

void Object::addCall(PTraceCall call)
{
    m_calls.insert(call);
    m_emitted = false;
}

void Object::addCall(const trace::Call& call)
{
    addCall(trace2call(call));
}

void Object::clearCalls()
{
    /* Don't remove init call */
    m_calls.clear();
}

bool Object::hasDependency(Pointer dep)
{
    auto iter = std::find(m_dependencies.begin(), m_dependencies.end(), dep);
    return iter != m_dependencies.end();
}

void Object::addDependency(Pointer dep)
{
    assert(dep);
    assert(!m_unrolled);
    m_dependencies.insert(dep);
    m_emitted = false;
}

void Object::addDependencies(Object::Pointer child, const trace::Array *deps)
{
    for (auto dep: deps->values) {
        child->addDependency(lookup<Object>(dep->toPointer()));
    }
}

void Object::moveTo(Pointer other)
{
    /* Don't move the init call */
    other->m_calls = std::move(m_calls);
    other->m_dependencies = std::move(m_dependencies);
    other->m_emitted = false;

    m_calls.clear();
    m_dependencies.clear();
}

void Object::copyTo(Pointer other)
{
    assert(!other->m_unrolled);

    other->m_calls.append(m_calls);
    other->m_dependencies.merge(other->m_dependencies);
    other->m_emitted = false;
    m_dependencies.clear();
}

void Object::emitInitCallsTo(CallSet& out_list)
{
    if (this->m_emitting)
        return;

    m_emitting = true;
    for (auto&& o: m_dependencies)
        o->emitInitCallsTo(out_list);
    out_list.insert(m_init_call);
    m_emitting = false;
}

void Object::emitCallsTo(CallSet& out_list, DepSet& dep_list)
{
    if (this->m_emitting)
        return;

    m_emitting = true;
    m_emitted = true;
    {
        out_list.insert(m_init_call);
        out_list.append(m_calls);

        for (auto&& o : m_dependencies) {
            if (o->m_unrolled)
                dep_list.insert(o);
            else
                o->emitCallsTo(out_list, dep_list);
        }

        emit(out_list, dep_list);
    }
    m_emitting = false;
}

void Object::emitCallsTo(CallSet& out_list)
{
    DepSet dep_list;

    if (this->m_emitting)
        return;

    emitCallsTo(out_list, dep_list);
    if (dep_list.empty())
        return;

    m_emitting = true;
    std::cout << id() << " before emitting unrolled deps: " << dep_list.size() << "\n";
    for (auto&& o: dep_list)
        o->emitCallsTo(out_list);
    m_emitting = false;
}

bool Object::emitted() const
{
    return m_emitted;
}

void Object::unroll()
{
    DepSet dep_list;

    for (auto&& o : m_dependencies) {
        if (o->m_unrolled)
            dep_list.insert(o);
        else
            o->emitCallsTo(m_calls, dep_list);
    }

    std::cout << id() << " unrolled from " << m_dependencies.size() << " to " << dep_list.size() << " deps\n";
    m_dependencies = std::move(dep_list);
    m_unrolled = true;
}

template <class T>
std::shared_ptr<T> Object::lookup(void* obj_id)
{
    return m_impl.lock()->lookup<T>(obj_id);
}

template <class T>
std::shared_ptr<T> Object::get(const trace::Call &call, unsigned obj_id_param_id)
{
    return m_impl.lock()->get<T>(call, obj_id_param_id);
}

template <class T, typename... Args>
std::shared_ptr<T> Object::create(const trace::Call &call, unsigned obj_id_param_id, Args... args)
{
    return m_impl.lock()->create<T>(call, shared_from_this(), obj_id_param_id, args...);
}

template <class T, typename... Args>
std::shared_ptr<T> Object::getOrCreate(const trace::Call &call, const char *obj_id_param, Args... args)
{
    auto arg_id = call.findArg(obj_id_param);
    return m_impl.lock()->getOrCreate<T>(call, shared_from_this(), arg_id, args...);
}

template <class T, typename... Args>
std::shared_ptr<T> Object::createWithDep(const trace::Call &call, unsigned obj_id_param,
                                 unsigned dep_id_param, Args... args)
{
    return m_impl.lock()->createWithDep<T>(call, shared_from_this(), obj_id_param, dep_id_param, args...);
}

template<class T, typename... Args>
std::shared_ptr<T> Object::fakeCreate(const trace::Call& call, Args... args)
{
    return m_impl.lock()->fakeCreate<T>(call, shared_from_this(), args...);
}

void
Object::callOnObject(const trace::Call& call, ePerDevice obj_type, unsigned obj_id_param)
{
    auto obj = get<Object>(call, obj_id_param);
    // XXX what about view <-> resource
    if (obj)
        obj->addCall(call);
}

void
Object::callOnObjectWithDep(const trace::Call& call, ePerDevice obj_type, unsigned obj_id_param,
                            ePerDevice dep_type, unsigned dep_id_param)
{
    auto obj = get<Object>(call, obj_id_param);
    auto dep = get<Object>(call, dep_id_param);

    // XXX what about view <-> resource
    if (obj && dep) {
        obj->addCall(call);
        obj->addDependency(dep);
    }
}

void Object::AddRef(const trace::Call& call)
{
    m_refcount++;
}

void Object::Release(const trace::Call& call)
{
    assert(m_refcount > 0);
    m_refcount--;
}

void Object::QueryInterface(const trace::Call& call)
{
    auto rrid = call.arg(1).toStruct();
    if (true) {
        getOrCreate<DXGIDevice>(call, "ppvObj");
    }
}

DXGIObject::DXGIObject(ImplPtr impl, void *id):
    Object(impl, id)
{
}

void
DXGIObject::GetParent(const trace::Call& call)
{
    getParent(call);
}

DXGIDevice::DXGIDevice(ImplPtr impl, void *id):
    DXGIObject(impl, id)
{
}

void
DXGIDevice::getParent(const trace::Call& call)
{
    getOrCreate<DXGIAdapter>(call, "ppParent");
}

DXGIAdapter::DXGIAdapter(ImplPtr impl, void *id):
    DXGIObject(impl, id)
{
}

void
DXGIAdapter::getParent(const trace::Call& call)
{
    getOrCreate<DXGIFactory>(call, "ppParent");
}

DXGIFactory::DXGIFactory(ImplPtr impl, void *id):
    DXGIObject(impl, id)
{
}

void
DXGIFactory::getParent(const trace::Call& call)
{
    // noop
}

void
DXGIFactory::CreateSwapChain(const trace::Call& call)
{
    auto desc = call.arg(2).toArray()->values[0]->toStruct();
    auto buffer_desc = desc->members[0]->toStruct();
    auto width = buffer_desc->members[0]->toUInt();
    auto height = buffer_desc->members[1]->toUInt();

    createWithDep<DXGISwapChain>(call, 3, 1, width, height);
}

void
DXGIFactory::CreateSwapChainForHwnd(const trace::Call& call)
{
    auto desc = call.arg(3).toArray()->values[0]->toStruct();
    auto width = desc->members[0]->toUInt();
    auto height = desc->members[1]->toUInt();

    createWithDep<DXGISwapChain>(call, 6, 1, width, height);
}

void DXGIFactory::EnumAdapters(const trace::Call& call)
{
    getOrCreate<DXGIAdapter>(call, "ppAdapter");
}

DXGISwapChain::DXGISwapChain(ImplPtr impl, void *id, unsigned width, unsigned height):
    DXGIObject(impl, id),
    m_width(width),
    m_height(height)
{
}

void
DXGISwapChain::getParent(const trace::Call& call)
{
    getOrCreate<DXGIFactory>(call, "ppParent");
}

void
DXGISwapChain::GetBuffer(const trace::Call& call)
{
    auto buf = getOrCreate<D3D11Texture2D>(call, "ppSurface", m_height, 0);
    if (!hasDependency(buf))
        addDependency(buf);
}

void
DXGISwapChain::ResizeBuffers(const trace::Call& call)
{
    m_width = call.arg(2).toUInt();
    m_height = call.arg(3).toUInt();
    addCall(call);
}

void
DXGISwapChain::ResizeTarget(const trace::Call& call)
{
    addCall(call);
}

void
DXGISwapChain::Present(const trace::Call& call)
{
    m_impl.lock()->recordObject(shared_from_this());
}

D3D11Device::D3D11Device(ImplPtr impl, void *id):
    Object(impl, id)
{
}

void
D3D11Device::GetImmediateContext(const trace::Call& call)
{
    getOrCreate<D3D11Context>(call, "ppImmediateContext");
}

void
D3D11Device::CreateDeferredContext(const trace::Call& call)
{
    create<D3D11Context>(call, 2, true);
}

void
D3D11Device::CreateState(const trace::Call& call)
{
    create<D3D11DeviceChild>(call, 2);
}

void
D3D11Device::CreateShader(const trace::Call& call)
{
    createWithDep<D3D11DeviceChild>(call, 4, 3);
}

void
D3D11Device::CreateGeometryShaderWithStreamOutput(const trace::Call& call)
{
    createWithDep<D3D11DeviceChild>(call, 9, 8);
}

void
D3D11Device::CreateBuffer(const trace::Call& call)
{
    /* D3D11_BUFFER_DESC.ByteWidth */
    unsigned size = call.arg(1).toArray()->values[0]->toStruct()->members[0]->toUInt();
    create<D3D11Buffer>(call, 3, size);
}

void
D3D11Device::CreateTexture1D(const trace::Call& call)
{
    /* Use byte size from map call */
    create<D3D11Texture1D>(call, 3);
}

void
D3D11Device::CreateTexture2D(const trace::Call& call)
{
    auto desc = call.arg(1).toArray()->values[0]->toStruct();
    /* D3D11_TEXTURE2D_DESC.Height */
    unsigned height = desc->members[1]->toUInt();
    /* D3D11_TEXTURE2D_DESC.BindFlags */
    unsigned bindFlags = desc->members[7]->toUInt();

    create<D3D11Texture2D>(call, 3, height, bindFlags);
}

void
D3D11Device::CreateTexture3D(const trace::Call& call)
{
    /* D3D11_TEXTURE3D_DESC.Depth */
    unsigned depth = call.arg(1).toArray()->values[0]->toStruct()->members[2]->toUInt();
    create<D3D11Texture3D>(call, 3, depth);
}

void
D3D11Device::CreateView(const trace::Call& call)
{
    auto res = get<D3D11Resource>(call, 1);
    if (!res) {
        std::cout << "resource for new view doesn't exist\n";
        return;
    }

    create<D3D11View>(call, 3, res);
}

void
D3D11Device::CreateAsync(const trace::Call& call)
{
    create<D3D11DeviceChild>(call, 2);
}

void
D3D11Device::CreateClassLinkage(const trace::Call& call)
{
    create<D3D11DeviceChild>(call, 1);
}

void
D3D11Device::CreateInputLayout(const trace::Call& call)
{
    create<D3D11DeviceChild>(call, 5);
}

D3D11DeviceChild::D3D11DeviceChild(ImplPtr impl, void *id):
    Object(impl, id)
{
}

D3D11State::D3D11State(const trace::Call& call)
{
    m_call = trace2call(call);
}

D3D11State::D3D11State(const trace::Call& call, D3D11DeviceChild::Pointer dep)
{
    m_call = trace2call(call);
    m_dep = dep;
}

D3D11State::D3D11State(const D3D11State& other)
{
    m_call = other.m_call;
    m_dep = other.m_dep;
}

void
D3D11State::addTo(Object::Pointer dst)
{
    dst->addCall(m_call);
    if (m_dep)
        dst->addDependency(m_dep);
}

D3D11Binding::D3D11Binding(ImplPtr impl, void *id, D3D11DeviceChild::Pointer bound_obj):
    D3D11DeviceChild(impl, id),
    m_bound_obj(bound_obj)
{
    if (bound_obj)
        addDependency(bound_obj);
}

template<class T>
std::shared_ptr<T>
D3D11Binding::get()
{
    return std::static_pointer_cast<T>(m_bound_obj);
}

D3D11Operation::D3D11Operation(ImplPtr impl, void *id, bool deferred):
    D3D11DeviceChild(impl, id),
    m_deferred(deferred)
{
}

void
D3D11Operation::addStates(States& states)
{
    for (auto&& [state_flag, state]: states)
        state.addTo(shared_from_this());

}

void
D3D11Operation::addBoundAsDependency(Bindings& bindings, enum ePerContextBinding pcb, bool compute=false)
{
    for (auto [bindpoint, slots]: bindings[pcb]) {
        bool is_compute = (bindpoint == ss_compute);
        if (compute != is_compute)
            return;

        for (auto [slot, binding]: slots) {
            if (!binding) {
                std::cout << "null bound dep\n";
                continue;
            }
            addDependency(binding);
        }
    }
}

void
D3D11Operation::addToBound(Bindings& bindings, enum ePerContextBinding pcb)
{
    for (auto [bindpoint, slots]: bindings[pcb]) {
        for (auto [slot, binding]: slots) {
            binding->addDependency(shared_from_this());
        }
    }
}

void
D3D11Operation::addToBoundView(Bindings& bindings, enum ePerContextBinding pcb, bool compute=false, FilterFn filter=nullptr)
{
    for (auto [bindpoint, slots]: bindings[pcb]) {
        bool is_compute = (bindpoint == ss_compute);
        if (compute != is_compute)
            return;

        for (auto [slot, binding]: slots) {
            auto view = binding->get<D3D11View>();
            if (!view)
                continue;
            if (filter && !filter(bindpoint, slot, view))
                continue;
            view->addUpdateDependency(shared_from_this());
        }
    }
}

D3D11Draw::D3D11Draw(ImplPtr impl, void *id, bool deferred):
    D3D11Operation(impl, id, deferred)
{
}

#define D3D11_BIND_SHADER_RESOURCE 0x8L

void
D3D11Draw::link(States& states, Bindings& bindings)
{
    addStates(states);

    addBoundAsDependency(bindings, pcb_shaders);
    addBoundAsDependency(bindings, pcb_samplers);
    addBoundAsDependency(bindings, pcb_input_layout);
    addBoundAsDependency(bindings, pcb_shader_resources);
    addBoundAsDependency(bindings, pcb_constant_buffers);
    addBoundAsDependency(bindings, pcb_vertex_buffers); // XXX take offset into consideration
    addBoundAsDependency(bindings, pcb_unordered_access_views);
    addBoundAsDependency(bindings, pcb_index_buffer); // XXX take offset into consideration
    addBoundAsDependency(bindings, pcb_render_targets);
    addBoundAsDependency(bindings, pcb_depth_stencil_view);

    auto check = [](unsigned bindpoint, unsigned slot, D3D11DeviceChild::Pointer obj) {
        if (!obj)
            return false;
        return std::static_pointer_cast<D3D11View>(obj)->hasBindFlag(D3D11_BIND_SHADER_RESOURCE);
    };
    addToBoundView(bindings, pcb_render_targets, false, check);
    //addToBoundView(bindings, pcb_depth_stencil_view); //  FIXME only if depth/stencil is enabled?
    //addToBoundView(bindings, pcb_unordered_access_views); // FIXME circular dependency?
    addToBound(bindings, pcb_stream_out_targets);
}

D3D11Dispatch::D3D11Dispatch(ImplPtr impl, void *id, bool deferred):
    D3D11Operation(impl, id, deferred)
{
}

void
D3D11Dispatch::link(States& states, Bindings& bindings)
{
    addBoundAsDependency(bindings, pcb_shaders, true);
    addBoundAsDependency(bindings, pcb_samplers, true);
    addBoundAsDependency(bindings, pcb_shader_resources, true);
    addBoundAsDependency(bindings, pcb_constant_buffers, true);
    addBoundAsDependency(bindings, pcb_unordered_access_views, true);

    addToBoundView(bindings, pcb_unordered_access_views);
}

D3D11Resource::D3D11Resource(ImplPtr impl, void *id):
    D3D11DeviceChild(impl, id)
{
}

void
D3D11Resource::addUpdateCall(const trace::Call& call)
{
    m_update_calls.push_back(trace2call(call));
    std::cout << this << " has " << m_update_calls.size() << " calls\n";
}

void
D3D11Resource::addUpdateDependency(Object::Pointer dep)
{
    assert(dep);
    m_update_dependencies.insert(dep);
    std::cout << id() << " has " << m_update_dependencies.size() << " dependencies\n";
}

void
D3D11Resource::update(const trace::Call& call, unsigned subres, D3D11Box *box, unsigned size)
{
    /* we only keep track of updates for buffers */
    addUpdateCall(call);
}

void
D3D11Resource::update(D3D11Mapping& mapping)
{
    if (mapping.discard)
        clearCalls();
    for (auto &&call: mapping.calls) {
        m_update_calls.push_back(call);
    }
}

void
D3D11Resource::clear()
{
    m_update_calls.clear();
    m_update_dependencies.clear();
}

void
D3D11Resource::emit(CallSet& out_list, DepSet& dep_list)
{
    for (auto &&n : m_update_calls)
        out_list.insert(n);
    m_update_calls.clear(); // XXX

    for (auto &&o : m_update_dependencies) {
        if (o->isUnrolled())
            dep_list.insert(o);
        else
            o->emitCallsTo(out_list, dep_list);
    }
}

D3D11Buffer::D3D11Buffer(ImplPtr impl, void *id, unsigned size):
    D3D11Resource(impl, id),
    m_size(size)
{
}

unsigned
D3D11Buffer::getMappedSize(unsigned width_stride, unsigned depth_stride)
{
    return m_size;
}

void
D3D11Buffer::update(const trace::Call& call, unsigned subres, D3D11Box *box, unsigned size)
{
    std::vector<PTraceCall> calls = { trace2call(call) };
    auto left = box ? box->left : 0;
    updateBuffer(calls, subres, left, left + size);
}

void
D3D11Buffer::update(D3D11Mapping& mapping)
{
    if (mapping.discard)
        clear();
    if (mapping.range_max > mapping.range_min)
        updateBuffer(mapping.calls, mapping.subres, mapping.range_min, mapping.range_max);
}

void
D3D11Buffer::updateBuffer(std::vector<PTraceCall> &calls, unsigned subres, unsigned begin, unsigned end)
{
    auto it = m_updates.begin();
    while (it != m_updates.end()) {
        auto update = *it;
        if (begin <= update.end && update.begin <= end) {
            if (begin > update.begin && end < update.end) {
                /* update in the middle of an existing update, let it go */
            } else if (begin > update.begin) {
                update.end = begin;
            } else if (end < update.end) {
                update.begin = end;
            } else {
                /* exact match; remove old update */
                it = m_updates.erase(it);
                continue;
            }
        }

        it++;
    }

    std::cout << "updating buffer from " << begin << " to " << end << "\n";
    Update update {begin, end, calls};
    m_updates.push_back(update);
}

void
D3D11Buffer::clear()
{
    D3D11Resource::clear();
    m_updates.clear();
}

void
D3D11Buffer::emit(CallSet& out_list, DepSet& dep_list)
{
    D3D11Resource::emit(out_list, dep_list);
    for (auto& update: m_updates) {
        for (auto&& call: update.calls)
            out_list.insert(call);
    }
}

D3D11Texture1D::D3D11Texture1D(ImplPtr impl, void *id):
    D3D11Resource(impl, id)
{
}

unsigned
D3D11Texture1D::getMappedSize(unsigned width_stride, unsigned depth_stride)
{
    return width_stride;
}

D3D11Texture2D::D3D11Texture2D(ImplPtr impl, void *id, unsigned height, unsigned bind_flags):
    D3D11Resource(impl, id),
    m_height(height),
    m_bind_flags(bind_flags)
{
}

unsigned
D3D11Texture2D::getMappedSize(unsigned width_stride, unsigned depth_stride)
{
    return m_height * width_stride;
}

D3D11Texture3D::D3D11Texture3D(ImplPtr impl, void *id, unsigned depth):
    D3D11Resource(impl, id),
    m_depth(depth)
{
}

unsigned
D3D11Texture3D::getMappedSize(unsigned width_stride, unsigned depth_stride)
{
    return m_depth * depth_stride;
}

D3D11View::D3D11View(ImplPtr impl, void *id, D3D11Resource::Pointer res):
    D3D11DeviceChild(impl, id),
    m_resource(res)
{
    addDependency(res);
}

void
D3D11View::addUpdateCall(const trace::Call& call)
{
    m_resource->addCall(getInitCall());
    m_resource->addUpdateCall(call);
}

void
D3D11View::addUpdateDependency(Object::Pointer dep)
{
    m_resource->addUpdateDependency(dep);
}

void
D3D11View::clear(const trace::Call& call)
{
    m_resource->clear();
    m_resource->addCall(getInitCall());
    m_resource->addUpdateCall(call);
}

D3D11Context::D3D11Context(ImplPtr impl, void *id, bool deferred):
    D3D11DeviceChild(impl, id),
    m_deferred(deferred)
{
}

void
D3D11Context::bindObject(const trace::Call& call, ePerContextBinding binding_type,
                         unsigned bindpoint, unsigned slot, void *bound_obj_id)
{
    auto& bindings = m_bindings[binding_type];
    auto bound_obj = lookup<D3D11DeviceChild>(bound_obj_id);

    // TODO cleanup
    auto binding = fakeCreate<D3D11Binding>(call, bound_obj);
    bindings[bindpoint][slot] = binding;

    if (bound_obj_id)
        assert(bound_obj);
    if (bound_obj_id == (void *)0x429f90 && m_impl.lock()->isRecording())
        std::cout << "fdfsdf\n";

    if (bound_obj)
        m_impl.lock()->recordObjectInit(bound_obj);
}

void
D3D11Context::bindObjects(const trace::Call& call, ePerContextBinding binding_type,
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
D3D11Context::bindSlot(const trace::Call& call, ePerContextBinding binding_type, unsigned param_id)
{
    auto res_id = call.arg(param_id).toPointer();
    bindObject(call, binding_type, 0, 0, res_id);
}

void
D3D11Context::bindSlots(const trace::Call& call, ePerContextBinding binding_type, unsigned bindpoint)
{
    auto slot = call.arg(1).toUInt();
    bindObjects(call, binding_type, bindpoint, slot, 3);
}


void
D3D11Context::ClearView(const trace::Call& call)
{
    auto view = get<D3D11View>(call, 1);
    view->clear(call);
}

void
D3D11Context::FinishCommandList(const trace::Call& call)
{
    // TODO auto restore = call.arg(1).toBool();
    auto cmdlist = create<D3D11CommandList>(call, 2);
    assert(m_deferred);
    moveTo(cmdlist);
    cmdlist->addCall(getInitCall());
}

void
D3D11Context::ExecuteCommandList(const trace::Call& call)
{
    // XXX Merge bindings and state
    auto cmdlist = get<D3D11CommandList>(call, 1);

    if (!m_deferred)
        cmdlist->unroll();
    m_impl.lock()->recordObject(cmdlist);
}

void
D3D11Context::SetShader(const trace::Call& call, unsigned bindpoint)
{
    auto shader_id = call.arg(1).toPointer();
    //XXX auto linkages = call.arg(2).toArray();
    //addDependencies(ctx, linkages);

    bindObject(call, pcb_shaders, bindpoint, 0, shader_id);
}

void
D3D11Context::BindRenderTargets(const trace::Call& call)
{
    bindObjects(call, pcb_render_targets, 0, 0, 2);
    bindSlot(call, pcb_depth_stencil_view, 3);
}

void
D3D11Context::BindRenderTargetsAndUAVS(const trace::Call& call)
{
    bindObjects(call, pcb_render_targets, 0, 0, 2);
    bindSlot(call, pcb_depth_stencil_view, 3);
    bindObjects(call, pcb_unordered_access_views, 0, 0, 5);
}

void
D3D11Context::GetBoundRenderTargets(const trace::Call& call)
{
    if (!m_impl.lock()->isRecording())
        return;

    // Make sure the last binding has been recorded
    for (auto [bindpoint, slots]: m_bindings[pcb_render_targets]) {
        for (auto [slot, binding]: slots) {
            m_impl.lock()->recordObject(binding);
        }
    }
    for (auto [bindpoint, slots]: m_bindings[pcb_depth_stencil_view]) {
        for (auto [slot, binding]: slots) {
            m_impl.lock()->recordObject(binding);
        }
    }
}

void
D3D11Context::SetState(const trace::Call& call, ePerContextState state_flag)
{
    m_states.insert_or_assign(state_flag, D3D11State(call));
}

void
D3D11Context::BindState(const trace::Call& call, ePerContextState state_flag)
{
    auto dep_id = call.arg(1).toPointer();

    if (dep_id) {
        auto dep = get<D3D11DeviceChild>(call, 1);
        m_states.insert_or_assign(state_flag, D3D11State(call, dep));
        m_impl.lock()->recordObject(dep);
    } else {
        m_states.insert_or_assign(state_flag, D3D11State(call));
    }
}

void
D3D11Context::ClearState(const trace::Call& call)
{
    // FIXME the call should probably be recorded
    m_states.clear();
    for (int i = 0; i < BINDING_TYPE_COUNT; i++)
        m_bindings[i].clear();
}

void
D3D11Context::Map(const trace::Call& call)
{
    auto res = get<D3D11Resource>(call, 1);
    auto subres = call.arg(2).toUInt();
    auto discard = (call.arg(3).toSInt() == 4);
    auto mapped_subres = call.arg(5).toArray()->values[0]->toStruct();
    auto begin = mapped_subres->members[0]->toUInt();
    auto row_pitch = mapped_subres->members[1]->toUInt();
    auto depth_pitch = mapped_subres->members[2]->toUInt();
    auto end = begin + res->getMappedSize(row_pitch, depth_pitch);
    auto mapping_id = std::make_tuple(id(), res->id(), subres);

    m_impl.lock()->recordObjectInit(res);
    m_impl.lock()->addMapping(call, mapping_id, begin, end, discard);
}

void
D3D11Context::Unmap(const trace::Call& call)
{
    auto res = get<D3D11Resource>(call, 1);
    auto subres = call.arg(2).toUInt();
    auto mapping_id = std::make_tuple(id(), res->id(), subres);

    m_impl.lock()->removeMapping(call, mapping_id);
}

void
D3D11Context::UpdateSubresource(const trace::Call& call)
{
    auto res = get<D3D11Resource>(call, 1);
    auto subres = call.arg(2).toUInt();
    auto box = toBox(call, 3);
    auto size = call.arg(4).toBlob()->size;

    /* noop if box is empty */
    if (!box || !box->isEmpty())
        res->update(call, subres, box, size);
    delete box;
}

void
D3D11Context::CopyResource(const trace::Call& call)
{
    auto dst = get<D3D11Resource>(call, 1);
    auto src = get<D3D11Resource>(call, 2);

    dst->clear();
    dst->addUpdateCall(call);
    dst->addUpdateDependency(src);
}

void
D3D11Context::CopySubresourceRegion(const trace::Call& call)
{
    auto dst = get<D3D11Resource>(call, 1);
    auto src = get<D3D11Resource>(call, 6);

    dst->addUpdateCall(call);
    dst->addUpdateDependency(src);
}

void
D3D11Context::CopyStructureCount(const trace::Call& call)
{
    auto dst = get<D3D11Buffer>(call, 1);
    auto src = get<D3D11View>(call, 3);

    dst->clear();
    dst->addUpdateCall(call);
    dst->addUpdateDependency(src);
}

void
D3D11Context::addOperation(D3D11Operation::Pointer op)
{
    // Emit list of calls right away for all dependencies of draw

    op->link(m_states, m_bindings);
    if (m_deferred) {
        addDependency(op);
    } else {
        op->unroll();
        m_impl.lock()->recordObject(op);
    }
}

void
D3D11Context::Draw(const trace::Call& call)
{
    auto draw = fakeCreate<D3D11Draw>(call, m_deferred);
    addOperation(draw);
}

void
D3D11Context::DrawIndirect(const trace::Call& call)
{
    auto draw = fakeCreate<D3D11Draw>(call, m_deferred);
    auto res = get<D3D11Buffer>(call, 1);
    draw->addDependency(res);
    addOperation(draw);
}

void
D3D11Context::Dispatch(const trace::Call& call)
{
    auto dispatch = fakeCreate<D3D11Dispatch>(call, m_deferred);
    addOperation(dispatch);
}

void
D3D11Context::DispatchIndirect(const trace::Call& call)
{
    auto dispatch = fakeCreate<D3D11Dispatch>(call, m_deferred);
    auto res = get<D3D11Buffer>(call, 1);
    dispatch->addDependency(res);
    addOperation(dispatch);
}

void
D3D11Context::Begin(const trace::Call& call)
{
    auto async = get<D3D11DeviceChild>(call, 1);
    async->addCall(call);
    m_impl.lock()->recordObject(async);
}

void
D3D11Context::End(const trace::Call& call)
{
    auto async = get<D3D11DeviceChild>(call, 1);
    async->addCall(call);
    m_impl.lock()->recordObject(async);
    async->clearCalls();
}

D3D11CommandList::D3D11CommandList(ImplPtr impl, void *id):
    D3D11DeviceChild(impl, id)
{
}

D3D11Impl::D3D11Impl(bool keep_all_states):
    FrameTrimmer(keep_all_states)
{
    registerInterfaces();
}

ft_callback
D3D11Impl::findCallback(const char *name)
{
    const char *del = strstr(name, "::");

    if (del) {
        /* Interface specific method */
        auto method_name = del + 2;
        auto iter = m_interfaces.find(name);
        if (iter == m_interfaces.end())
            return nullptr;
        return iter->second->findCall(method_name);
    } else {
        /* Global method */
        auto iter = m_call_table.find(name);
        if (iter == m_call_table.end())
            return nullptr;
        return iter->second;
    }
}

void D3D11Impl::emitState()
{
}

// Map callbacks to call methods of FrameTrimImpl
#define MAP(name, ...) \
    { \
        auto f = [&](const trace::Call& call) { \
            this->name(call, ## __VA_ARGS__); \
        }; \
        m_call_table.insert(std::make_pair(#name, f)); \
    }

#define BEGIN_INTERFACE(name, classname, inherit) \
    do { \
        using Class = classname; \
        auto parent = m_interfaces[#inherit]; \
        auto interface = std::make_shared<Interface>(#name, parent); \
        m_interfaces[#name] = interface;

#define END_INTERFACE() \
    } while (0);

#define METHOD_AS(name, func, ...) \
    { \
        auto f = [&](const trace::Call& call) { \
            auto obj_id = call.arg(0).toPointer(); \
            auto obj = std::static_pointer_cast<Class>(m_objects[obj_id]); \
            assert(obj); \
            obj->func(call, ## __VA_ARGS__); \
        }; \
        interface->addCall(#name, f); \
    }

#define METHOD(name, ...) \
    METHOD_AS(name, name, __VA_ARGS__)

void D3D11Impl::registerInterfaces()
{
    // top-level interface
    m_interfaces.insert({"None", std::make_shared<Interface>("None")});

    MAP(CreateDXGIFactory);
    MAP(D3D11CreateDevice);
    MAP(D3D11CreateDeviceAndSwapChain);
    MAP(memcpy);

    BEGIN_INTERFACE(IUnknown, Object, None)
        METHOD(AddRef)
        METHOD(Release)
        METHOD(QueryInterface)
    END_INTERFACE()

    BEGIN_INTERFACE(IDXGIObject, DXGIObject, IUnknown)
        METHOD(GetParent)
    END_INTERFACE()

    BEGIN_INTERFACE(IDXGIFactory, DXGIFactory, IDXGIObject)
        METHOD(CreateSwapChain);
        METHOD(CreateSwapChainForHwnd);
        METHOD(EnumAdapters);
        METHOD_AS(EnumAdapters1, EnumAdapters);
        // METHOD(EnumAdapterByLuuid, enumAdapter)
        // METHOD(EnumWarpAdapter, enumAdapter)
    END_INTERFACE()

    BEGIN_INTERFACE(IDXGIDevice, DXGIDevice, IDXGIObject)
    END_INTERFACE()

    BEGIN_INTERFACE(IDXGIAdapter, DXGIAdapter, IDXGIObject)
    END_INTERFACE()

    BEGIN_INTERFACE(IDXGISwapChain, DXGISwapChain, IDXGIObject)
        METHOD(ResizeBuffers);
        METHOD(ResizeTarget);
        METHOD(GetBuffer);
        METHOD(Present);
    END_INTERFACE()

    BEGIN_INTERFACE(ID3D11Device, D3D11Device, IUnknown)
        METHOD(GetImmediateContext);
        METHOD(CreateDeferredContext);

        METHOD_AS(CreateBlendState, CreateState);
        METHOD_AS(CreateDepthStencilState, CreateState);
        METHOD_AS(CreateRasterizerState, CreateState);
        METHOD_AS(CreateSamplerState, CreateState);

        METHOD_AS(CreateVertexShader, CreateShader)
        METHOD_AS(CreateHullShader, CreateShader)
        METHOD_AS(CreateDomainShader, CreateShader)
        METHOD_AS(CreateGeometryShader, CreateShader)
        METHOD_AS(CreatePixelShader, CreateShader)
        METHOD_AS(CreateComputeShader, CreateShader)
        METHOD(CreateGeometryShaderWithStreamOutput)

        METHOD(CreateBuffer);
        METHOD(CreateTexture1D);
        METHOD(CreateTexture2D);
        METHOD(CreateTexture3D);

        METHOD_AS(CreateShaderResourceView, CreateView);
        METHOD_AS(CreateUnorderedAccessView, CreateView);
        METHOD_AS(CreateRenderTargetView, CreateView);
        METHOD_AS(CreateDepthStencilView, CreateView);

        METHOD_AS(CreateQuery, CreateAsync);
        METHOD_AS(CreateCounter, CreateAsync);
        METHOD_AS(CreatePredicate, CreateAsync);

        METHOD(CreateClassLinkage);
        METHOD(CreateInputLayout);
    END_INTERFACE()

    BEGIN_INTERFACE(ID3D11DeviceContext, D3D11Context, IUnknown)
        METHOD(FinishCommandList);
        METHOD(ExecuteCommandList);

        METHOD(ClearState);
        //TODO METHOD(Flush, addCall);

        METHOD_AS(IASetIndexBuffer, bindSlot, pcb_index_buffer, 1);
        METHOD_AS(IASetVertexBuffers, bindSlots, pcb_vertex_buffers, ss_vertex);
        METHOD_AS(IASetInputLayout, bindSlot, pcb_input_layout, 1);
        METHOD_AS(IASetPrimitiveTopology, SetState, pc_primitive_topology);

        METHOD_AS(OMSetBlendState, BindState, pc_blend);
        METHOD_AS(OMSetDepthStencilState, BindState, pc_depth_stencil);
        METHOD_AS(OMSetRenderTargets, BindRenderTargets);
        METHOD_AS(OMSetRenderTargetsAndUnorderedAccessViews, BindRenderTargetsAndUAVS);
        METHOD_AS(OMGetRenderTargets, GetBoundRenderTargets);

        METHOD_AS(RSSetViewports, SetState, pc_viewport);
        METHOD_AS(RSSetScissorRects, SetState, pc_scissor);
        METHOD_AS(RSSetState, BindState, pc_rasterizer);

        METHOD_AS(SOSetTargets, bindObjects, pcb_stream_out_targets, 0, 0, 2);

        METHOD_AS(VSSetShader, SetShader, ss_vertex);
        METHOD_AS(HSSetShader, SetShader, ss_hull);
        METHOD_AS(DSSetShader, SetShader, ss_domain);
        METHOD_AS(GSSetShader, SetShader, ss_geometry);
        METHOD_AS(PSSetShader, SetShader, ss_pixel);
        METHOD_AS(CSSetShader, SetShader, ss_compute);

        METHOD_AS(VSSetSamplers, bindSlots, pcb_samplers, ss_vertex);
        METHOD_AS(HSSetSamplers, bindSlots, pcb_samplers, ss_hull);
        METHOD_AS(DSSetSamplers, bindSlots, pcb_samplers, ss_domain);
        METHOD_AS(GSSetSamplers, bindSlots, pcb_samplers, ss_geometry);
        METHOD_AS(PSSetSamplers, bindSlots, pcb_samplers, ss_pixel);
        METHOD_AS(CSSetSamplers, bindSlots, pcb_samplers, ss_compute);

        METHOD_AS(VSSetShaderResources, bindSlots, pcb_shader_resources, ss_vertex);
        METHOD_AS(HSSetShaderResources, bindSlots, pcb_shader_resources, ss_hull);
        METHOD_AS(DSSetShaderResources, bindSlots, pcb_shader_resources, ss_domain);
        METHOD_AS(GSSetShaderResources, bindSlots, pcb_shader_resources, ss_geometry);
        METHOD_AS(PSSetShaderResources, bindSlots, pcb_shader_resources, ss_pixel);
        METHOD_AS(CSSetShaderResources, bindSlots, pcb_shader_resources, ss_compute);

        METHOD_AS(VSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_vertex);
        METHOD_AS(HSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_hull);
        METHOD_AS(DSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_domain);
        METHOD_AS(GSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_geometry);
        METHOD_AS(PSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_pixel);
        METHOD_AS(CSSetConstantBuffers, bindSlots, pcb_constant_buffers, ss_compute);

        METHOD_AS(CSSetUnorderedAccessViews, bindSlots, pcb_unordered_access_views, ss_compute);

        METHOD_AS(ClearRenderTargetView, ClearView);
        METHOD_AS(ClearDepthStencilView, ClearView);
        METHOD_AS(ClearUnorderedAccessViewFloat, ClearView);
        METHOD_AS(ClearUnorderedAccessViewUint, ClearView);

        METHOD(Map);
        METHOD(Unmap);
        METHOD_AS(UpdateResource, callOnObject, pd_resources, 1);
        METHOD(UpdateSubresource);
        METHOD(CopyResource);
        METHOD(CopyStructureCount);
        METHOD(CopySubresourceRegion);
        METHOD_AS(GenerateMips, callOnObject, pd_views, 1);

    #if 0
        METHOD(SetPredication);
        METHOD(GetData);
    #endif
        METHOD(Begin); // query, predicate and counter
        METHOD(End);

        METHOD(Draw);
        METHOD_AS(DrawAuto, Draw);
        METHOD_AS(DrawIndexed, Draw);
        METHOD_AS(DrawIndexedInstanced, Draw);
        METHOD_AS(DrawInstanced, Draw);
        METHOD_AS(DrawIndexedInstancedIndirect, DrawIndirect);
        METHOD_AS(DrawInstancedIndirect, DrawIndirect);

        METHOD(Dispatch);
        METHOD(DispatchIndirect);
    END_INTERFACE()

    BEGIN_INTERFACE(ID3D11Buffer, D3D11Buffer, IUnknown)
    END_INTERFACE()

    BEGIN_INTERFACE(ID3D11Texture1D, D3D11Texture1D, IUnknown)
    END_INTERFACE()

    BEGIN_INTERFACE(ID3D11Texture2D, D3D11Texture2D, IUnknown)
    END_INTERFACE()

    BEGIN_INTERFACE(ID3D11Texture3D, D3D11Texture3D, IUnknown)
    END_INTERFACE()

    BEGIN_INTERFACE(ID3D11CommandList, D3D11CommandList, IUnknown)
    END_INTERFACE()
}

bool D3D11Impl::skipDeleteObj(const trace::Call& call)
{
    const char *del = strstr(call.name(), "::");

    if (!del)
        return false;

    auto method_name = del + 2;
    if (strcmp(method_name, "Release") != 0)
        return false;

    /*auto obj_id = call.arg(0).toPointer();
    auto ret = lookup<Object>(obj_id);
    if (ret)
        return false;*/

    // TODO add back Release() call at the end of the frame if we try to loop
    // the last frame?

    return true;
}

void
D3D11Impl::recordObject(Object::Pointer obj)
{
    if (!obj)
        return;
    if (m_recording_frame)
        obj->emitCallsTo(m_required_calls);
}

void
D3D11Impl::recordObjectInit(Object::Pointer obj)
{
    if (m_recording_frame)
        obj->emitInitCallsTo(m_required_calls);
}

template<class T>
std::shared_ptr<T>
D3D11Impl::lookup(void* obj_id)
{
    return std::static_pointer_cast<T>(m_objects[obj_id]);
}

template<class T>
std::shared_ptr<T>
D3D11Impl::get(const trace::Call& call, unsigned obj_id_param_id)
{
    auto obj_id = call.arg(obj_id_param_id).toPointer();
    return lookup<T>(obj_id);
}

template<class T, typename... Args>
std::shared_ptr<T>
D3D11Impl::create(const trace::Call& call, Object::Pointer parent, unsigned obj_id_param_id, Args... args)
{
    if (call.ret->toSInt() != 0)
        return std::shared_ptr<T>();

    auto id = unwrapObjAt(call, obj_id_param_id);

    auto obj = std::make_shared<T>(shared_from_this(), id, args...);
    obj->setInitCall(call);
    if (parent)
        obj->addDependency(parent);
    m_objects[id] = obj;

    return obj;
}

template<class T, typename... Args>
std::shared_ptr<T>
D3D11Impl::createWithDep(const trace::Call& call, Object::Pointer parent,
                         unsigned obj_id_param, unsigned dep_id_param,
                         Args... args)
{
    auto obj = create<T>(call, parent, obj_id_param, args...);

    auto dep_id = call.arg(dep_id_param).toPointer();
    if (dep_id) {
        auto dep = m_objects[dep_id];
        obj->addDependency(dep);
    }

    return obj;
}

template<class T, typename... Args>
std::shared_ptr<T>
D3D11Impl::getOrCreate(const trace::Call& call, Object::Pointer parent, unsigned obj_id_param_id, Args... args)
{
    auto obj_id = unwrapObjAt(call, obj_id_param_id);
    auto ret = lookup<T>(obj_id);

    if (ret)
        return ret;
    return create<T>(call, parent, obj_id_param_id, args...);
}

template<class T, typename... Args>
std::shared_ptr<T>
D3D11Impl::fakeCreate(const trace::Call& call, Object::Pointer parent, Args... args)
{
    /* TODO explain */
    auto obj = make_shared<T>(shared_from_this(), (void *)call.no, args...);
    obj->setInitCall(call);
    obj->addDependency(parent);
    return obj;
}

void
D3D11Impl::CreateDXGIFactory(const trace::Call& call)
{
    auto arg_id = call.findArg("ppFactory");
    create<DXGIFactory>(call, std::shared_ptr<Object>(), arg_id);
}

void
D3D11Impl::D3D11CreateDevice(const trace::Call& call)
{
    auto dev = createWithDep<D3D11Device>(call, std::shared_ptr<Object>(), 7, 0);
    dev->GetImmediateContext(call);
}

void
D3D11Impl::D3D11CreateDeviceAndSwapChain(const trace::Call& call)
{
    auto dev = createWithDep<D3D11Device>(call, std::shared_ptr<Object>(), 10, 0);
    dev->GetImmediateContext(call);
}

void D3D11Impl::addMapping(const trace::Call& call, SubresourceId id,
                           unsigned long long start, unsigned long long end,
                           bool discard)
{
    auto subres = std::get<2>(id);
    m_buffer_mappings.insert(std::make_pair(id, D3D11Mapping(call, subres, start, end, discard)));
}

void D3D11Impl::removeMapping(const trace::Call& call, SubresourceId id)
{
    auto iter = m_buffer_mappings.find(id);
    if (iter == m_buffer_mappings.end()) {
        std::cerr << "Error: can't unmap unknown buffer mapping\n";
        return;
    }
    auto mapping = iter->second;
    mapping.finish(call);

    auto res = lookup<D3D11Resource>(std::get<1>(id));
    res->update(mapping);

    m_buffer_mappings.erase(iter);
}

void D3D11Impl::memcpy(const trace::Call& call)
{
    auto start = call.arg(0).toUInt();
    auto end = start + call.arg(2).toUInt();

    for (auto&& [id, mapping]: m_buffer_mappings) {
        if (mapping.start <= start && start < mapping.end) {
            if (end > mapping.end) {
                std::cerr << "\n:Error "<< call.no << "(memcpy): Mapped target range is ["
                          << mapping.start << ", " << mapping.end << "] but data requires ["
                          << start << ", " << end << "]\n";
                assert(0);
            }

            mapping.update(call, start, end);
            return;
        }
    }

    std::cerr << "Found no mapping for memcopy to " << start << " in call " << call.no << ": " << call.name() << "\n";
    assert(0);
}

}
