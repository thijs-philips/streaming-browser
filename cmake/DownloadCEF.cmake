function(DownloadPinnedCEF platform version expected_sha1 download_dir)
  set(distribution "cef_binary_${version}_${platform}")
  set(root "${download_dir}/${distribution}")
  set(archive "${download_dir}/${distribution}.tar.bz2")
  string(REPLACE "+" "%2B" escaped_distribution "${distribution}")
  set(url "https://cef-builds.spotifycdn.com/${escaped_distribution}.tar.bz2")

  if(NOT IS_DIRECTORY "${root}")
    file(MAKE_DIRECTORY "${download_dir}")
    if(NOT EXISTS "${archive}")
      message(STATUS "Downloading pinned CEF ${version} for ${platform}")
      if(WIN32)
        execute_process(
          COMMAND curl.exe --ssl-no-revoke --fail --location --retry 3
                  --output "${archive}" "${url}"
          RESULT_VARIABLE download_result)
        if(NOT download_result EQUAL 0)
          file(REMOVE "${archive}")
          message(FATAL_ERROR "Failed to download pinned CEF archive")
        endif()
      else()
        file(DOWNLOAD
          "${url}"
          "${archive}"
          SHOW_PROGRESS
          TLS_VERIFY ON
          STATUS download_status)
        list(GET download_status 0 download_result)
        if(NOT download_result EQUAL 0)
          file(REMOVE "${archive}")
          message(FATAL_ERROR "Failed to download pinned CEF archive: ${download_status}")
        endif()
      endif()
    endif()

    file(SHA1 "${archive}" actual_sha1)
    if(NOT actual_sha1 STREQUAL expected_sha1)
      file(REMOVE "${archive}")
      message(FATAL_ERROR "CEF archive hash mismatch; removed ${archive}")
    endif()

    message(STATUS "Extracting pinned CEF archive")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xjf "${archive}"
      WORKING_DIRECTORY "${download_dir}"
      RESULT_VARIABLE extract_result)
    if(NOT extract_result EQUAL 0 OR NOT IS_DIRECTORY "${root}")
      message(FATAL_ERROR "Failed to extract CEF archive: ${archive}")
    endif()
  endif()

  set(CEF_ROOT "${root}" CACHE INTERNAL "Pinned CEF root" FORCE)
endfunction()
