# EROFS - Enhanced Read-Only File System
## This is a personal repo. More info: [EROFS](https://docs.kernel.org/filesystems/erofs.html)

EROFS is a high-performance file system for read-only situations. Currently, the erofs-utils user mode tool packs and builds images (mkfs.erofs) in a single-threaded way, and the unpacking of fsck.erofs is also single-threaded. There is room for improvement in end-to-end time for multi-threaded compression/decompression optimization. 


EROFS currently supports l4 and lzma algorithms. For different algorithms, it is necessary to design a matching multi-threading strategy to improve concurrency efficiency. At the same time, multi-thread development needs to balance the overhead of scheduling and computing to avoid regression. Packaging tools hope to maintain the requirements of reproducible builds on the basis of multi-threaded optimization. 


Finally, the erofs-utils multi-threaded unpacking and packaging can be expanded with the number of multi-processor cores, and it is required that the multi-processor scene can improve the lzma algorithm by up to 50%.
