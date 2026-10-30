#pragma once

namespace HuffmanGzip
{
	/*!
	* Gzips with zlib's Huffman-only strategy: no LZ77 match search.
	*
	* A GME reply is AES-encrypted and base64'd before it is sent, so it has no
	* repeated strings for LZ77 to find; all gzip saves on it is base64's 6 bits
	* per character, which Huffman coding alone gets.  On a 100 KB reply that is
	* the same 75% size as drogon's gzip (level 6) for about a ninth of the CPU
	* (~0.3 ms instead of ~3 ms).  The output is an ordinary gzip stream, so the
	* client's curl (Accept-Encoding: gzip) decodes it unchanged.
	*
	* @param[in] input Bytes to compress
	* @return The gzip stream, or nothing if zlib failed
	*/
	std::optional<std::string> Compress(std::string_view input);
}
