#include "App.hpp"
#include "HuffmanGzip.hpp"

#include <zlib.h>

std::optional<std::string> HuffmanGzip::Compress(std::string_view input)
{
	z_stream stream{};
	// MAX_WBITS + 16 writes a gzip header and trailer instead of zlib's.  The
	// level does not matter under Z_HUFFMAN_ONLY; memLevel 8 is zlib's default.
	if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, MAX_WBITS + 16, 8, Z_HUFFMAN_ONLY) != Z_OK)
		return std::nullopt;

	std::string output;
	output.resize(deflateBound(&stream, static_cast<uLong>(input.size())));
	stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
	stream.avail_in = static_cast<uInt>(input.size());
	stream.next_out = reinterpret_cast<Bytef*>(output.data());
	stream.avail_out = static_cast<uInt>(output.size());

	// deflateBound covers the whole stream, so one Z_FINISH completes it.
	const int result = deflate(&stream, Z_FINISH);
	output.resize(stream.total_out);
	deflateEnd(&stream);
	if (result != Z_STREAM_END)
		return std::nullopt;
	return output;
}
