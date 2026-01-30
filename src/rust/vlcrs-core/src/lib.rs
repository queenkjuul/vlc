#![deny(unsafe_op_in_unsafe_fn)]
#![feature(extern_types)]
#![feature(associated_type_defaults)]
#![feature(allocator_api)]
#![feature(c_size_t)]

//! The `vlcrs-core` crate.
//!
//! This crate contains the vlc core APIs that have been ported or
//! wrapped for usage by Rust code in the modules and is shared by all of them.
//!
//! If you need a vlc core C API that is not ported or wrapped yet here,
//! then do so first instead of bypassing this crate.

// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2024 Alexandre Janniaux <ajanni@videolabs.io>
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation; either version 2.1 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.#![deny(unsafe_op_in_unsafe_fn)]

pub mod plugin;

pub mod object;

pub mod tracer;

pub(crate) mod convert;

pub type RawVLCError = libc::c_int;

#[derive(Clone, Copy, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct Error(RawVLCError);

use std::io::ErrorKind;
impl Error {

    pub fn from_kind(kind: ErrorKind) -> Self {
        match kind {
            ErrorKind::ArgumentListTooLong => Self(-libc::E2BIG),
            ErrorKind::PermissionDenied => Self(-libc::EACCES),
            // TODO
            _ => Self(Self::EGENERIC),
        }
    }

    pub fn kind(&self) -> ErrorKind {
        let Some(errno) = self.0.checked_abs() else {
            return ErrorKind::Other;
        };

        match errno {
            libc::E2BIG => ErrorKind::ArgumentListTooLong,
            libc::EACCES => ErrorKind::PermissionDenied,
            _ => ErrorKind::Other,
        }
    }

    pub(crate) fn from_raw(raw: RawVLCError) -> Self {
        Self(raw)
    }

    pub fn as_raw(&self) -> RawVLCError {
        self.0
    }

    pub const EGENERIC: RawVLCError = RawVLCError::MIN;
}

impl std::fmt::Display for Error {
    fn fmt(&self, fmt: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self.kind() {
        ErrorKind::Other => {
                let code = self.0;
                if code == Self::EGENERIC {
                    write!(fmt, "legacy generic VLC error")
                } else {
                    write!(fmt, "unmapped error ({code})")
                }
            }
            kind => kind.fmt(fmt),
        }
    }
}

impl std::fmt::Debug for Error {
    fn fmt(&self, fmt: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let mut fmt = fmt.debug_struct("VLCError");
        match self.kind() {
            ErrorKind::Other => {
                let code = self.0;
                if code == Self::EGENERIC {
                    fmt.field("kind", &"LegacyGeneric")
                } else {
                    fmt.field("kind", &"Unmapped").field("code", &code)
                }
            }
            kind => fmt.field("kind", &kind),
        }
        .finish()
    }
}

impl From<std::io::Error> for Error {
    fn from(value: std::io::Error) -> Self {
        Error::from_kind(value.kind())
    }
}

impl std::error::Error for Error {}

#[cfg(test)]
mod test {
    use std::io::ErrorKind;

    use crate::Error;

    #[test]
    fn test_legacy_vlc_generic_err() {
        assert_eq!(Error::EGENERIC, libc::c_int::MIN);
        let e = Error(Error::EGENERIC);
        assert_eq!(e.kind(), ErrorKind::Other);

        match e.kind() {
            ErrorKind::Other if e.as_raw() == Error::EGENERIC => todo!(),
            _ => unreachable!(),
        }
    }
}
