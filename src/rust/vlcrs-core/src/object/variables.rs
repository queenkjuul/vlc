use std::ffi::CStr;

use crate::object::sys;

use super::Object;

pub trait Variable<T> {
    fn create(&self, name: &CStr);
    fn inherit(&self, name: &CStr) -> Option<T>;
    fn get(&self, name: &CStr) -> Option<T>;
    fn set(&self, name: &CStr, val: T);
}

impl Variable<i64> for Object<'_> {
    fn create(&self, name: &CStr) {
        unsafe {
            sys::var_Create(self, name.as_ptr() as _, sys::VLC_VAR_INTEGER);
        }
    }

    fn inherit(&self, name: &CStr) -> Option<i64> {
        let mut value: sys::vlc_value_t = unsafe { std::mem::zeroed() };
        let ret =
            unsafe { sys::var_Inherit(self, name.as_ptr() as _, sys::VLC_VAR_INTEGER, &mut value) };

        if ret == 0 {
            Some(unsafe { value.i_int })
        } else {
            None
        }
    }

    fn get(&self, name: &CStr) -> Option<i64> {
        let mut value: sys::vlc_value_t = unsafe { std::mem::zeroed() };
        let ret =
            unsafe { sys::var_Get(self, name.as_ptr() as _, sys::VLC_VAR_INTEGER, &mut value) };

        if ret == 0 {
            Some(unsafe { value.i_int })
        } else {
            None
        }
    }

    fn set(&self, name: &CStr, val: i64) {
        let value = sys::vlc_value_t{ i_int: val };
        unsafe {
            sys::var_Set(self, name.as_ptr(), &value);
        }
    }
}

impl Variable<f32> for Object<'_> {
    fn create(&self, name: &CStr) {
        unsafe {
            sys::var_Create(self, name.as_ptr() as _, sys::VLC_VAR_FLOAT);
        }
    }

    fn inherit(&self, name: &CStr) -> Option<f32> {
        let mut value: sys::vlc_value_t = unsafe { std::mem::zeroed() };
        let ret =
            unsafe { sys::var_Inherit(self, name.as_ptr() as _, sys::VLC_VAR_FLOAT, &mut value) };

        if ret == 0 {
            Some(unsafe { value.f_float })
        } else {
            None
        }
    }

    fn get(&self, name: &CStr) -> Option<f32> {
        let mut value: sys::vlc_value_t = unsafe { std::mem::zeroed() };
        let ret =
            unsafe { sys::var_Get(self, name.as_ptr() as _, sys::VLC_VAR_FLOAT, &mut value) };

        if ret == 0 {
            Some(unsafe { value.f_float })
        } else {
            None
        }
    }

    fn set(&self, name: &CStr, val: f32) {
        let value = sys::vlc_value_t{ f_float: val };
        unsafe {
            sys::var_Set(self, name.as_ptr(), &value);
        }
    }
}

//impl Variable<f32> for Object<'_> {
//    fn inherit(&self, name: &CStr) -> Option<f32> {
//        todo!()
//    }
//
//    fn get(&self, name: &CStr) -> Option<f32> {
//        todo!()
//    }
//}
//
//impl Variable<String> for Object<'_> {
//    fn inherit(&self, name: &CStr) -> Option<String> {
//        todo!()
//    }
//
//    fn get(&self, name: &CStr) -> Option<String> {
//        todo!()
//    }
//}

#[cfg(test)]
mod test {
    use crate::object::{Object, ObjectHandle};

    use super::Variable;


    #[test]
    fn test_variable() {
        let obj = ObjectHandle::new(None).unwrap();

        assert_eq!(obj.get(c"foo"), Option::<i64>::None);

        <Object as Variable<i64>>::create(&obj, c"foo");
        assert_eq!(obj.get(c"foo"), Option::<i64>::None);
        obj.set(c"foo",  42i64);
        assert_eq!(obj.get(c"foo"), Some(42i64));
        obj.destroy_variable(c"foo");
    }
}
