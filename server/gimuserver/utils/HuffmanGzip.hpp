#pragma once

namespace HuffmanGzip
{
	/*!
	* Gzips with zlib's Huffman-only strategy: no LZ77 match search.
	*
	* A GME reply is AES-encrypted and base64'd before it is sent, so it has no
	* repeated strings for LZ77 to find; all gzip saves on it is base64's 6 bits
	* per character, which Huffman coding alone gets.  On such a reply that is
	* the same 75% size as drogon's gzip (level 6) for a fraction of the CPU.
	* The output is an ordinary gzip stream, so the client's curl
	* (Accept-Encoding: gzip) decodes it unchanged.
	*
	* @param[in] input Bytes to compress
	* @return The gzip stream, or nothing if zlib failed
	*/
	std::optional<std::string> Compress(std::string_view input);

	/*!
	* Sets a response body, gzipped with Compress() under the same conditions
	* drogon's own gzip uses (use_gzip on, at least 1024 bytes, the request
	* accepts gzip).  Drogon leaves a response that already has a
	* Content-Encoding alone, so the body is not compressed twice; anything else
	* still gets drogon's gzip.
	*
	* @param[in] rq Request being answered
	* @param[in] resp Response to fill
	* @param[in] body Uncompressed body
	*/
	void SetBody(const drogon::HttpRequestPtr& rq, const drogon::HttpResponsePtr& resp, std::string body);
}
