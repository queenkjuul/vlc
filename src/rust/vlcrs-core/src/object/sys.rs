use crate::object::Object;
use core::ffi::c_int;
use std::marker::PhantomData;
use std::ptr::NonNull;

#[repr(C)]
pub(super) struct ObjectInternals {
    _unused: [u8; 0],
}

#[repr(C)]
pub(super) struct ObjectMarker {
    _unused: [u8; 0],
}

#[repr(C)]
#[derive(Copy, Clone)]
pub(super) union ObjectInternalData<'parent> {
    pub internals: Option<NonNull<ObjectInternals>>,
    pub marker: Option<NonNull<ObjectMarker>>,
    pub _parent: PhantomData<&'parent Object<'parent>>,
}

pub const VLC_VAR_VOID: c_int = 0x0010;
pub const VLC_VAR_BOOL: c_int = 0x0020;
pub const VLC_VAR_INTEGER: c_int = 0x0030;
pub const VLC_VAR_STRING: c_int = 0x0040;
pub const VLC_VAR_FLOAT: c_int = 0x0050;
pub const VLC_VAR_ADDRESS: c_int = 0x0070;
pub const VLC_VAR_COORDS: c_int = 0x00A0;

#[repr(C)]
#[derive(Copy, Clone)]
pub(super) union vlc_value_t {
    pub i_int: i64,
    pub b_bool: bool,
    pub f_float: core::ffi::c_float,
    pub psz_string: *mut core::ffi::c_char,
    pub p_address: *mut core::ffi::c_void,
}

extern "C" {

    ///
    /// Create a VLC object, with an optional parent.
    ///
    /// For now, the lifetime is more constrained than necessary so that
    /// the function can be used in test without creating unsound situations.
    ///
    pub(crate) fn vlc_object_create<'parent, 'child>(
        parent: Option<&'_ Object<'parent>>,
        length: core::ffi::c_size_t,
    ) -> Option<NonNull<Object<'child>>>
    where
        'parent: 'child;

    pub(crate) fn vlc_object_delete(obj: &mut Object);

    pub(crate) fn var_Create(obj: &Object, name: *const core::ffi::c_char, type_: c_int) -> c_int;

    pub(crate) fn var_Destroy(obj: &Object, name: *const core::ffi::c_char) -> c_int;

    pub(crate) fn var_Set(
        obj: &Object,
        name: *const core::ffi::c_char,
        value: *const vlc_value_t,
    ) -> c_int;

    pub(crate) fn var_Get(
        obj: &Object,
        name: *const core::ffi::c_char,
        type_: c_int,
        value: *mut vlc_value_t,
    ) -> c_int;

    pub(crate) fn var_Inherit(
        obj: &Object,
        name: *const core::ffi::c_char,
        type_: c_int,
        value: *mut vlc_value_t,
    ) -> c_int;
}
