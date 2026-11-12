# Modified for Cara on 2026-11-12; see NOTICE for attribution.
function(copy_cara_FFmpeg_deps target_dir)
    include(WindowsCopyFiles)
    set(DLL_DEST "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/")
    windows_copy_files(${target_dir} ${FFMPEG_DIR}/bin ${DLL_DEST}
        avcodec*.dll
        avformat*.dll
        avutil*.dll
        swresample*.dll
        swscale*.dll
    )
endfunction(copy_cara_FFmpeg_deps)
