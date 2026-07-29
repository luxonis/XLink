use std::env;
use std::path::{Path, PathBuf};

/// Collects sources with the given extension from a directory (non recursive),
/// mirroring the file(GLOB ...) logic from cmake/XLink.cmake.
fn glob_sources(dir: &Path, extension: &str, exclude: &[&str]) -> Vec<PathBuf> {
    let mut sources: Vec<PathBuf> = std::fs::read_dir(dir)
        .unwrap_or_else(|err| panic!("failed to read {}: {err}", dir.display()))
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| path.extension().is_some_and(|ext| ext == extension))
        .filter(|path| {
            let name = path.file_name().unwrap().to_string_lossy();
            !exclude.contains(&name.as_ref())
        })
        .collect();
    sources.sort();
    sources
}

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    // When building from a published crate, the C/C++ sources are vendored
    // into `xlink-src/` (see .github/workflows/rust-bindings.yml). When
    // building from the repository, they live at the repository root.
    let vendored = manifest_dir.join("xlink-src");
    let root = if vendored.join("src").is_dir() {
        vendored
    } else {
        // <XLink root>/rust/xlink-sys -> <XLink root>
        manifest_dir
            .ancestors()
            .nth(2)
            .expect("xlink-sys must live in <XLink root>/rust/xlink-sys")
            .to_path_buf()
    };

    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_env = env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();
    let libusb_enabled = env::var("CARGO_FEATURE_LIBUSB").is_ok();

    let src_pc = root.join("src/pc");
    let src_protocols = root.join("src/pc/protocols");
    let src_shared = root.join("src/shared");

    println!("cargo:rerun-if-changed={}", root.join("src").display());
    println!("cargo:rerun-if-changed={}", root.join("include").display());

    let mut usb_excludes: Vec<&str> = Vec::new();
    if !libusb_enabled {
        // Same removal as cmake/XLink.cmake when XLINK_ENABLE_LIBUSB=OFF
        usb_excludes.push("usb_host.cpp");
        usb_excludes.push("win_usb_host.cpp");
    }

    let mut c_sources = Vec::new();
    let mut cpp_sources = Vec::new();

    c_sources.extend(glob_sources(&src_pc, "c", &usb_excludes));
    cpp_sources.extend(glob_sources(&src_pc, "cpp", &usb_excludes));
    c_sources.extend(glob_sources(&src_protocols, "c", &usb_excludes));
    cpp_sources.extend(glob_sources(&src_protocols, "cpp", &usb_excludes));
    // file(GLOB_RECURSE ...) on src/shared; the directory is currently flat
    c_sources.extend(glob_sources(&src_shared, "c", &[]));
    cpp_sources.extend(glob_sources(&src_shared, "cpp", &[]));

    let mut platform_include: Option<PathBuf> = None;
    match target_os.as_str() {
        "windows" => {
            platform_include = Some(root.join("src/pc/Win/include"));
            c_sources.extend(glob_sources(
                &root.join("src/pc/Win/src"),
                "c",
                &usb_excludes,
            ));
            cpp_sources.extend(glob_sources(
                &root.join("src/pc/Win/src"),
                "cpp",
                &usb_excludes,
            ));
        }
        "macos" | "ios" => {
            platform_include = Some(root.join("src/pc/MacOS"));
            c_sources.push(root.join("src/pc/MacOS/pthread_semaphore.c"));
        }
        _ => {}
    }

    // Stub for the header normally produced by CMake's generate_export_header.
    // We always build a static library, so the macros expand to nothing.
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let export_header_dir = out_dir.join("generated-include");
    std::fs::create_dir_all(&export_header_dir).unwrap();
    std::fs::write(
        export_header_dir.join("XLinkExport.h"),
        "#ifndef XLINK_EXPORT_H\n\
         #define XLINK_EXPORT_H\n\
         #define XLINK_EXPORT\n\
         #define XLINK_NO_EXPORT\n\
         #define XLINK_DEPRECATED\n\
         #endif\n",
    )
    .unwrap();

    let configure_common = |build: &mut cc::Build| {
        build
            .include(root.join("include"))
            .include(root.join("include/XLink"))
            .include(&export_header_dir)
            .include(&src_protocols)
            .define("HAVE_STRUCT_TIMESPEC", None)
            .define("_CRT_SECURE_NO_WARNINGS", None)
            .define("USE_USB_VSC", None)
            .define("USE_TCP_IP", None)
            .warnings(false);

        if let Some(include) = &platform_include {
            build.include(include);
        }

        if libusb_enabled {
            build.define("XLINK_ENABLE_LIBUSB", None);
        }

        match target_os.as_str() {
            "windows" => {
                build.define("WIN32_LEAN_AND_MEAN", None);
            }
            "linux" | "android" => {
                build.define("_GNU_SOURCE", None);
                // pthread_getname_np is available on glibc >= 2.12; musl gained
                // it in 1.2.3 but keep the conservative glibc-only default.
                if target_env == "gnu" {
                    build.define("HAVE_PTHREAD_GETNAME_NP", None);
                }
            }
            "macos" | "ios" => {
                build.define("HAVE_PTHREAD_GETNAME_NP", None);
            }
            _ => {}
        }
    };

    let mut usb_include: Option<PathBuf> = None;
    if libusb_enabled {
        #[allow(unused_mut)]
        let mut probed = pkg_config::Config::new()
            .atleast_version("1.0")
            .probe("libusb-1.0");
        match probed {
            Ok(library) => {
                usb_include = library.include_paths.first().cloned();
            }
            Err(err) => {
                panic!("the `libusb` feature requires libusb-1.0 (pkg-config lookup failed: {err})")
            }
        }
    }

    // C sources (C99, gnu extensions like the CMake build)
    let mut c_build = cc::Build::new();
    configure_common(&mut c_build);
    if let Some(include) = &usb_include {
        c_build.include(include);
    }
    c_build.std("gnu99").files(&c_sources).compile("xlink_c");

    // C++ sources (C++11)
    let mut cpp_build = cc::Build::new();
    configure_common(&mut cpp_build);
    if let Some(include) = &usb_include {
        cpp_build.include(include);
    }
    cpp_build
        .cpp(true)
        .std("c++11")
        .files(&cpp_sources)
        .compile("xlink_cpp");

    match target_os.as_str() {
        "windows" => {
            println!("cargo:rustc-link-lib=ws2_32");
            println!("cargo:rustc-link-lib=iphlpapi");
        }
        "macos" | "ios" => {}
        "android" => {
            println!("cargo:rustc-link-lib=log");
        }
        _ => {
            println!("cargo:rustc-link-lib=pthread");
        }
    }
}
