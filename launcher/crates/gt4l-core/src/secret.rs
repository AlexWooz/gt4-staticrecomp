//! Secret protection abstraction and Windows DPAPI adapter for RetroAchievements credentials.
//!
//! Windows-only DPAPI (`CryptProtectData`/`CryptUnprotectData`) binds ciphertext to the current
//! Windows user and machine. Non-Windows implementations fail explicitly without falling back
//! to plaintext persistence or silent fake providers.

use std::fmt;

pub const DPAPI_V1_PREFIX: &str = "dpapi:v1:";
pub const FAKE_V1_PREFIX: &str = "fake:v1:";

/// Errors occurring during secret protection or unprotection.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SecretError {
    /// The current operating system platform does not support the requested protection backend.
    UnsupportedPlatform(String),
    /// The underlying OS crypto API failed (e.g., CryptProtectData or CryptUnprotectData).
    OsError { code: u32, message: String },
    /// The ciphertext envelope is malformed, has an unknown scheme, or contains corrupted data.
    CorruptData(String),
    /// The crypto backend service was unavailable or rejected the operation.
    BackendUnavailable(String),
}

impl fmt::Display for SecretError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnsupportedPlatform(msg) => write!(f, "unsupported platform for secret protection: {msg}"),
            Self::OsError { code, message } => write!(f, "OS crypto error 0x{code:08X}: {message}"),
            Self::CorruptData(msg) => write!(f, "corrupted protected secret: {msg}"),
            Self::BackendUnavailable(msg) => write!(f, "secret protection backend unavailable: {msg}"),
        }
    }
}

impl std::error::Error for SecretError {}

/// Platform-neutral interface for protecting secrets at rest.
pub trait SecretProtector: Send + Sync {
    /// Protects a secret string into a versioned envelope string.
    fn protect_secret(&self, secret: &str) -> Result<String, SecretError>;
    /// Unprotects a versioned envelope string back into the original secret string.
    fn unprotect_secret(&self, envelope: &str) -> Result<String, SecretError>;
}

/// Encodes binary data to lowercase hexadecimal ASCII.
pub fn hex_encode(data: &[u8]) -> String {
    let mut s = String::with_capacity(data.len() * 2);
    for byte in data {
        use std::fmt::Write;
        let _ = write!(&mut s, "{:02x}", byte);
    }
    s
}

fn hex_val(b: u8) -> Result<u8, SecretError> {
    match b {
        b'0'..=b'9' => Ok(b - b'0'),
        b'a'..=b'f' => Ok(b - b'a' + 10),
        b'A'..=b'F' => Ok(b - b'A' + 10),
        _ => Err(SecretError::CorruptData("invalid non-hexadecimal character in ciphertext envelope".into())),
    }
}

/// Decodes hexadecimal ASCII into binary data.
///
/// Operates strictly on byte slices to guarantee safe handling of arbitrary malformed,
/// multi-byte UTF-8, or truncated inputs without slicing panics or secret leakage.
pub fn hex_decode(s: &str) -> Result<Vec<u8>, SecretError> {
    let bytes = s.trim().as_bytes();
    if bytes.len() % 2 != 0 {
        return Err(SecretError::CorruptData("hexadecimal length must be an even number".into()));
    }
    let mut out = Vec::with_capacity(bytes.len() / 2);
    for chunk in bytes.chunks_exact(2) {
        let high = hex_val(chunk[0])?;
        let low = hex_val(chunk[1])?;
        out.push((high << 4) | low);
    }
    Ok(out)
}

/// Real Windows DPAPI secret protector using `CryptProtectData` and `CryptUnprotectData`.
#[derive(Clone, Copy, Debug, Default)]
pub struct DpapiProtector;

impl SecretProtector for DpapiProtector {
    fn protect_secret(&self, secret: &str) -> Result<String, SecretError> {
        #[cfg(windows)]
        {
            let ciphertext = dpapi::protect(secret.as_bytes())?;
            Ok(format!("{DPAPI_V1_PREFIX}{}", hex_encode(&ciphertext)))
        }
        #[cfg(not(windows))]
        {
            let _ = secret;
            Err(SecretError::UnsupportedPlatform("DPAPI is only available on Windows".into()))
        }
    }

    fn unprotect_secret(&self, envelope: &str) -> Result<String, SecretError> {
        let trimmed = envelope.trim();
        if !trimmed.starts_with(DPAPI_V1_PREFIX) {
            return Err(SecretError::CorruptData(format!(
                "unsupported secret envelope scheme (expected prefix {DPAPI_V1_PREFIX})"
            )));
        }
        let hex_payload = &trimmed[DPAPI_V1_PREFIX.len()..];
        if hex_payload.is_empty() {
            return Err(SecretError::CorruptData("secret envelope contains empty payload".into()));
        }
        let ciphertext = hex_decode(hex_payload)?;
        if ciphertext.is_empty() {
            return Err(SecretError::CorruptData("secret envelope contains empty payload".into()));
        }

        #[cfg(windows)]
        {
            let plaintext_bytes = dpapi::unprotect(&ciphertext)?;
            String::from_utf8(plaintext_bytes)
                .map_err(|e| SecretError::CorruptData(format!("decrypted secret is not valid UTF-8: {e}")))
        }
        #[cfg(not(windows))]
        {
            let _ = ciphertext;
            Err(SecretError::UnsupportedPlatform("DPAPI is only available on Windows".into()))
        }
    }
}

/// Default system protector (DPAPI on Windows, explicit error on other platforms).
pub fn default_protector() -> DpapiProtector {
    DpapiProtector
}

/// Injectable fake secret protector for deterministic unit and error-injection testing.
#[derive(Clone, Debug)]
pub struct FakeProtector {
    pub key: u8,
    pub fail_protect: bool,
    pub fail_unprotect: bool,
    pub wrong_user_code: u32,
}

impl Default for FakeProtector {
    fn default() -> Self {
        Self {
            key: 0x5A,
            fail_protect: false,
            fail_unprotect: false,
            wrong_user_code: 0x8009000B, // NTE_BAD_KEYSET
        }
    }
}

impl SecretProtector for FakeProtector {
    fn protect_secret(&self, secret: &str) -> Result<String, SecretError> {
        if self.fail_protect {
            return Err(SecretError::BackendUnavailable("injected fake protection failure".into()));
        }
        let mut xored = Vec::with_capacity(secret.len());
        for b in secret.as_bytes() {
            xored.push(b ^ self.key);
        }
        Ok(format!("{FAKE_V1_PREFIX}{}", hex_encode(&xored)))
    }

    fn unprotect_secret(&self, envelope: &str) -> Result<String, SecretError> {
        if self.fail_unprotect {
            return Err(SecretError::OsError {
                code: self.wrong_user_code,
                message: "injected fake unprotection failure: keyset does not exist or user mismatch".into(),
            });
        }
        let trimmed = envelope.trim();
        if !trimmed.starts_with(FAKE_V1_PREFIX) {
            return Err(SecretError::CorruptData(format!(
                "unsupported fake secret envelope scheme (expected prefix {FAKE_V1_PREFIX})"
            )));
        }
        let hex_payload = &trimmed[FAKE_V1_PREFIX.len()..];
        if hex_payload.is_empty() {
            return Err(SecretError::CorruptData("fake secret envelope contains empty payload".into()));
        }
        let xored = hex_decode(hex_payload)?;
        if xored.is_empty() {
            return Err(SecretError::CorruptData("fake secret envelope contains empty payload".into()));
        }
        let mut plaintext = Vec::with_capacity(xored.len());
        for b in xored {
            plaintext.push(b ^ self.key);
        }
        String::from_utf8(plaintext)
            .map_err(|e| SecretError::CorruptData(format!("decrypted fake secret is not valid UTF-8: {e}")))
    }
}

#[cfg(windows)]
mod dpapi {
    use super::SecretError;

    #[repr(C)]
    struct DataBlob {
        cb_data: u32,
        pb_data: *mut u8,
    }

    const CRYPTPROTECT_UI_FORBIDDEN: u32 = 0x1;

    #[link(name = "crypt32")]
    #[link(name = "kernel32")]
    extern "system" {
        fn CryptProtectData(
            pDataIn: *const DataBlob,
            szDataDescr: *const u16,
            pOptionalEntropy: *const DataBlob,
            pvReserved: *mut std::ffi::c_void,
            pPromptStruct: *mut std::ffi::c_void,
            dwFlags: u32,
            pDataOut: *mut DataBlob,
        ) -> i32;

        fn CryptUnprotectData(
            pDataIn: *const DataBlob,
            ppszDataDescr: *mut *mut u16,
            pOptionalEntropy: *const DataBlob,
            pvReserved: *mut std::ffi::c_void,
            pPromptStruct: *mut std::ffi::c_void,
            dwFlags: u32,
            pDataOut: *mut DataBlob,
        ) -> i32;

        fn LocalFree(hMem: *mut std::ffi::c_void) -> *mut std::ffi::c_void;
        fn GetLastError() -> u32;
    }

    pub fn protect(plaintext: &[u8]) -> Result<Vec<u8>, SecretError> {
        let dummy = 0u8;
        let in_blob = DataBlob {
            cb_data: plaintext.len() as u32,
            pb_data: if plaintext.is_empty() {
                &dummy as *const u8 as *mut u8
            } else {
                plaintext.as_ptr() as *mut u8
            },
        };
        let mut out_blob = DataBlob {
            cb_data: 0,
            pb_data: std::ptr::null_mut(),
        };

        let ok = unsafe {
            CryptProtectData(
                &in_blob,
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut out_blob,
            )
        };

        if ok == 0 {
            let code = unsafe { GetLastError() };
            return Err(SecretError::OsError {
                code,
                message: format!("CryptProtectData failed with code 0x{code:08X}"),
            });
        }

        let slice = unsafe { std::slice::from_raw_parts(out_blob.pb_data, out_blob.cb_data as usize) };
        let result = slice.to_vec();
        unsafe {
            LocalFree(out_blob.pb_data as *mut std::ffi::c_void);
        }
        Ok(result)
    }

    pub fn unprotect(ciphertext: &[u8]) -> Result<Vec<u8>, SecretError> {
        if ciphertext.is_empty() {
            return Err(SecretError::CorruptData("ciphertext blob is empty".into()));
        }
        let in_blob = DataBlob {
            cb_data: ciphertext.len() as u32,
            pb_data: ciphertext.as_ptr() as *mut u8,
        };
        let mut out_blob = DataBlob {
            cb_data: 0,
            pb_data: std::ptr::null_mut(),
        };

        let ok = unsafe {
            CryptUnprotectData(
                &in_blob,
                std::ptr::null_mut(),
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut out_blob,
            )
        };

        if ok == 0 {
            let code = unsafe { GetLastError() };
            return Err(SecretError::OsError {
                code,
                message: format!("CryptUnprotectData failed with code 0x{code:08X}"),
            });
        }

        let slice = unsafe { std::slice::from_raw_parts(out_blob.pb_data, out_blob.cb_data as usize) };
        let result = slice.to_vec();
        unsafe {
            LocalFree(out_blob.pb_data as *mut std::ffi::c_void);
        }
        Ok(result)
    }
}
