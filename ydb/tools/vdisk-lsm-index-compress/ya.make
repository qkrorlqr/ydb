PROGRAM(vdisk-lsm-index-compress)

PEERDIR(
    library/cpp/getopt
    library/cpp/streams/bzip2
)

SRCS(
    compressed_index.cpp
    error.cpp
    huffman.cpp
    main.cpp
    record.cpp
    snapshot.cpp
)

END()
