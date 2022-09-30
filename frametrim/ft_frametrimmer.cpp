/*********************************************************************
 *
 * Copyright 2020 Collabora Ltd
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

#include "ft_frametrimmer.hpp"
#include "ft_d3d11.hpp"
#include "ft_opengl.hpp"

#include "trace_model.hpp"

#include <algorithm>

namespace frametrim {

FrameTrimmer::FrameTrimmer(bool keep_all_states):
    m_keep_all_state_calls(keep_all_states),
    m_recording_frame(false),
    m_last_frame_start(0)
{
}

std::unique_ptr<FrameTrimmer>
FrameTrimmer::create(trace::API api, bool keep_all_states)
{
    if (api == trace::API_GL || api == trace::API_EGL) {
        std::cerr << "OpenGL trimmer\n";
        return std::unique_ptr<FrameTrimmer>(new OpenGLImpl(keep_all_states));
    } else if (api == trace::API_DXGI || api == trace::API_UNKNOWN) {
        std::cerr << "D3D11 trimmer\n";
        return std::unique_ptr<FrameTrimmer>(new D3D11Impl(keep_all_states));
    } else {
        assert(0);
    }
}

void FrameTrimmer::start_last_frame(uint32_t callno)
{
    std::cerr << "\n---> Start last frame at call no " << callno << "\n";
    m_last_frame_start = callno;
}

void
FrameTrimmer::call(const trace::Call& call, Frametype frametype)
{
    const char *call_name = call.name();

    if (!m_recording_frame && (frametype != ft_none)) {
        std::cerr << "Start recording\n";
        m_recording_frame = true;
    }
    if (m_recording_frame && (frametype == ft_none)) {
        std::cerr << "End recording\n";
        m_recording_frame = false;
    }

    /* Skip delete calls for objects that have never been emitted, or
     * if we are in the last frame and the object was created in an earlier frame.
     * By not deleting such objects looping the last frame will work in more cases */
    if (skipDeleteObj(call)) {
        return;
    }

    auto icb = m_call_table_cache.find(call.name());
    if (icb != m_call_table_cache.end())
        icb->second(call);
    else {
        auto cb_range = m_call_table.equal_range(call.name());
        if (cb_range.first != m_call_table.end() &&
                std::distance(cb_range.first, cb_range.second) > 0) {

            CallTable::const_iterator cb = cb_range.first;
            CallTable::const_iterator i = cb_range.first;
            ++i;

            unsigned max_equal = equalChars(cb->first, call_name);

            while (i != cb_range.second && i != m_call_table.end()) {
                auto n = equalChars(i->first, call_name);
                if (n > max_equal) {
                    max_equal = n;
                    cb = i;
                }
                ++i;
            }

            if (max_equal) {
                //if (strcmp(call.name(), cb->first))
                //    std::cerr << "Handle " << call.name() << " as " << cb->first << "\n";
                cb->second(call);
                m_call_table_cache[call.name()] = cb->second;
            } else {
                if (m_unhandled_calls.find(call_name) == m_unhandled_calls.end()) {
                    std::cerr << "Call " << call.no
                              << " " << call_name << " not handled\n";
                    m_unhandled_calls.insert(call_name);
                }
            }
        } else  if (!(call.flags & trace::CALL_FLAG_END_FRAME)) {
            /* This should be some debug output only, because we might
             * not handle some calls deliberately */
            if (m_unhandled_calls.find(call_name) == m_unhandled_calls.end()) {
                std::cerr << "Call " << call.no
                          << " " << call_name << " not handled\n";
                m_unhandled_calls.insert(call_name);
            }
        }
    }

    auto c = trace2call(call);

    if (frametype == ft_none) {
        if (call.flags & trace::CALL_FLAG_END_FRAME)
            m_last_swap = c;
    } else {
        if (!(call.flags & trace::CALL_FLAG_END_FRAME)) {
            m_required_calls.insert(c);
        } else {
            if (frametype == ft_retain_frame) {
                m_required_calls.insert(c);
                if (m_last_swap) {
                    m_required_calls.insert(m_last_swap);
                    m_last_swap = nullptr;
                }
            } else
                m_last_swap = c;
        }
    }
}

void FrameTrimmer::finalize()
{
    if (m_last_swap)
        m_required_calls.insert(m_last_swap);
}

std::unordered_set<unsigned>
FrameTrimmer::getUniqueCallIds()
{
    std::unordered_set<unsigned> retval;
    for(auto&& c: m_required_calls)
        retval.insert(c->callNo());
    return retval;
}

std::vector<unsigned>
FrameTrimmer::getSortedCallIds()
{
    auto make_sure_its_singular = getUniqueCallIds();

    std::vector<unsigned> sorted_calls(
                make_sure_its_singular.begin(),
                make_sure_its_singular.end());
    std::sort(sorted_calls.begin(), sorted_calls.end());
    return sorted_calls;
}

unsigned
FrameTrimmer::equalChars(const char *prefix, const char *callname)
{
    unsigned retval = 0;

    const char *prefix_del = strstr(prefix, "::");
    const char *callname_del = strstr(callname, "::");
    if (prefix_del && callname_del) {
        prefix = prefix_del;
        callname = callname_del;
    }

    while (*prefix && *callname && *prefix == *callname) {
        ++retval;
        ++prefix; ++callname;
    }
    if (!*prefix && !*callname)
        ++retval;

    return !*prefix ? retval : 0;
}

}
