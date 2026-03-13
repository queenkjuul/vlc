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

macro_rules! error_kind_map {
    ($($(#[$meta:meta])* $kind:ident => $errno:ident $(| $alias:ident)*),+ $(,)?) => {
        pub fn from_kind(kind: ErrorKind) -> Self {
            match kind {
                $($(#[$meta])* ErrorKind::$kind => Self::from_errno(libc::$errno),)+
                _ => Self::EGENERIC,
            }
        }

        pub fn kind(&self) -> ErrorKind {
            let Some(errno) = self.0.checked_abs() else {
                return ErrorKind::Other;
            };

            #[allow(unreachable_patterns)]
            match errno {
                $($(#[$meta])* libc::$errno $(| libc::$alias)* => ErrorKind::$kind,)+
                _ => ErrorKind::Other,
            }
        }
    };
}

impl Error {
    error_kind_map! {
        ArgumentListTooLong    => E2BIG,
        AddrInUse              => EADDRINUSE,
        AddrNotAvailable       => EADDRNOTAVAIL,
        ResourceBusy           => EBUSY,
        ConnectionAborted      => ECONNABORTED,
        ConnectionRefused      => ECONNREFUSED,
        ConnectionReset        => ECONNRESET,
        Deadlock               => EDEADLK,
        QuotaExceeded          => EDQUOT,
        AlreadyExists          => EEXIST,
        FileTooLarge           => EFBIG,
        HostUnreachable        => EHOSTUNREACH,
        Interrupted            => EINTR,
        InvalidInput           => EINVAL,
        IsADirectory           => EISDIR,
        NotFound               => ENOENT,
        OutOfMemory            => ENOMEM,
        StorageFull            => ENOSPC,
        Unsupported            => ENOSYS | EOPNOTSUPP,
        TooManyLinks           => EMLINK,
        InvalidFilename        => ENAMETOOLONG,
        NetworkDown            => ENETDOWN,
        NetworkUnreachable     => ENETUNREACH,
        NotConnected           => ENOTCONN,
        NotADirectory          => ENOTDIR,
        DirectoryNotEmpty      => ENOTEMPTY,
        BrokenPipe             => EPIPE,
        ReadOnlyFilesystem     => EROFS,
        NotSeekable            => ESPIPE,
        StaleNetworkFileHandle => ESTALE,
        TimedOut               => ETIMEDOUT,
        ExecutableFileBusy     => ETXTBSY,
        CrossesDevices         => EXDEV,
        PermissionDenied       => EACCES | EPERM,
        WouldBlock             => EAGAIN | EWOULDBLOCK,
    }

    fn from_errno(errno: libc::c_int) -> Self {
        Self(-errno)
    }

    pub(crate) fn from_raw(raw: RawVLCError) -> Self {
        Self(raw)
    }

    pub fn as_raw(&self) -> RawVLCError {
        self.0
    }

    pub const EGENERIC: Error = Error(RawVLCError::MIN);
}

impl std::fmt::Display for Error {
    fn fmt(&self, fmt: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self.kind() {
            ErrorKind::Other => {
                let code = self.0;
                if *self == Self::EGENERIC {
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
                if *self == Self::EGENERIC {
                    fmt.field("kind", &"LegacyGeneric")
                } else {
                    fmt.field("kind", &"Other").field("code", &code)
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
        assert_eq!(Error::EGENERIC.as_raw(), libc::c_int::MIN);
        let e = Error::EGENERIC;
        assert_eq!(e.kind(), ErrorKind::Other);

        match e.kind() {
            ErrorKind::Other if e == Error::EGENERIC => todo!(),
            _ => unreachable!(),
        }
    }
}
