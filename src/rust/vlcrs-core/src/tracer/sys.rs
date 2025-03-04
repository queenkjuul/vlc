#![allow(rustdoc::bare_urls)]
#![allow(rustdoc::broken_intra_doc_links)]
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

/// SPDX-License-Identifier: LGPL-2.1-or-later
/// Copyright (C) 2024-2025 Alexandre Janniaux <ajanni@videolabs.io>
///
/// This program is free software; you can redistribute it and/or modify it
/// under the terms of the GNU Lesser General Public License as published by
/// the Free Software Foundation; either version 2.1 of the License, or
/// (at your option) any later version.
///
/// This program is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
/// GNU Lesser General Public License for more details.
///
/// You should have received a copy of the GNU Lesser General Public License
/// along with this program; if not, write to the Free Software Foundation,
/// Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
use std::{
    borrow::Cow,
    ffi::{c_char, c_double, c_void, CStr},
    marker::PhantomData,
    ptr::NonNull,
};

use crate::object::Object;

pub type vlc_tick = i64;

#[repr(C)]
#[non_exhaustive]
#[doc = "Tracer message values"]
#[derive(Debug, Copy, Clone, Hash, PartialEq, Eq)]
pub enum vlc_tracer_value_type {
    Integer = 0,
    Double = 1,
    String = 2,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub union vlc_tracer_value {
    pub integer: i64,
    pub double: c_double,
    pub string: *const c_char,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct vlc_tracer_entry {
    pub key: *const c_char,
    pub value: vlc_tracer_value,
    pub kind: vlc_tracer_value_type,
}

#[repr(C)]
pub struct vlc_tracer_trace {
    pub entries: NonNull<vlc_tracer_entry>,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct vlc_tracer {
    _unused: [u8; 0],
}

#[repr(transparent)]
pub struct Trace<'a>(&'a vlc_tracer_trace);

impl<'a> Trace<'a> {
    pub fn iter(&self) -> TraceIterator<'a> {
        TraceIterator {
            current_field: self.0.entries,
            _plt: PhantomData,
        }
    }
}

impl<'a> IntoIterator for Trace<'a> {
    type Item = TraceEntry<'a>;
    type IntoIter = TraceIterator<'a>;

    fn into_iter(self) -> Self::IntoIter {
        let entries = self.0.entries;
        TraceIterator {
            current_field: entries,
            _plt: PhantomData,
        }
    }
}

pub enum TraceValue<'a> {
    Integer(i64),
    Double(f64),
    String(Cow<'a, str>),
}

pub struct TraceEntry<'a> {
    pub key: Cow<'a, str>,
    pub value: TraceValue<'a>,
}

impl<'a> From<vlc_tracer_entry> for TraceEntry<'a> {
    fn from(entry: vlc_tracer_entry) -> Self {
        // SAFETY: Key is guaranteed to be non-null by the iterator.
        let key = unsafe { CStr::from_ptr(entry.key) }.to_string_lossy();

        let value = unsafe {
            match entry.kind {
                vlc_tracer_value_type::Integer => TraceValue::Integer(entry.value.integer),
                vlc_tracer_value_type::Double => TraceValue::Double(entry.value.double),
                vlc_tracer_value_type::String => {
                    TraceValue::String(CStr::from_ptr(entry.value.string).to_string_lossy())
                }
            }
        };

        Self { key, value }
    }
}

pub struct TraceIterator<'a> {
    pub(crate) current_field: NonNull<vlc_tracer_entry>,
    pub(crate) _plt: PhantomData<&'a vlc_tracer_entry>,
}

impl<'a> Iterator for TraceIterator<'a> {
    type Item = TraceEntry<'a>;

    fn next(&mut self) -> Option<Self::Item> {
        unsafe {
            if self.current_field.read().key.is_null() {
                return None;
            }
            let output = Some(TraceEntry::from(self.current_field.read()));
            self.current_field = self.current_field.add(1);
            output
        }
    }
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct vlc_tracer_operations {
    pub trace: extern "C" fn(opaque: *const c_void, ts: vlc_tick, trace: Trace),
    pub destroy: extern "C" fn(*mut c_void),
}

extern "C" {
    pub fn vlc_tracer_Create(parent: &Object, name: *const c_char) -> Option<NonNull<vlc_tracer>>;
    pub fn vlc_tracer_TraceWithTs(tracer: NonNull<vlc_tracer>, tick: vlc_tick, trace: Trace);
}

#[cfg(test)]
mod test {
    #[test]
    fn test_trace_interop() {
        use super::*;
        let entries = [
            vlc_tracer_entry {
                kind: vlc_tracer_value_type::String,
                value: vlc_tracer_value {
                    string: c"value1".as_ptr(),
                },
                key: c"test1".as_ptr(),
            },
            vlc_tracer_entry {
                kind: vlc_tracer_value_type::Integer,
                value: vlc_tracer_value { integer: 0 },
                key: std::ptr::null(),
            },
        ];

        //let trace_field = TraceField {
        //    entry: NonNull::from(&entries[0]),
        //};
        //assert_eq!(trace_field.kind(), vlc_tracer_value_type::String);
        //assert_eq!(trace_field.key(), "test1");

        //let trace_field = TraceField {
        //    entry: NonNull::from(&entries[1]),
        //};
        //assert_eq!(trace_field.kind(), vlc_tracer_value_type::Integer);

        let trace = vlc_tracer_trace {
            entries: NonNull::from(&entries[0]),
        };

        let trace = Trace(&trace);
        let mut iterator = trace.iter();

        let first = iterator.next().expect("First field must be valid");
        //assert_eq!(first.kind(), vlc_tracer_value_type::String);
        //assert_eq!(first.key(), "test1");
        //unsafe {
        //    assert_eq!(CStr::from_ptr(first.value().string), c"value1");
        //}

        //let second = iterator.next();
        //assert_eq!(second, None);
    }
}
