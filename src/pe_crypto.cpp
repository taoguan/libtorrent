/*

Copyright (c) 2007-2016, Un Shyam, Arvid Norberg, Steven Siloti
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#if !defined(TORRENT_DISABLE_ENCRYPTION) && !defined(TORRENT_DISABLE_EXTENSIONS)

#include <cstdint>
#include <algorithm>
#include <random>

#include "libtorrent/aux_/disable_warnings_push.hpp"

#include <boost/multiprecision/integer.hpp>
#include <boost/multiprecision/cpp_int.hpp>

// for backwards compatibility with boost < 1.60 which was before export_bits
// and import_bits were introduced
#if BOOST_VERSION < 106000
#include "libtorrent/aux_/cppint_import_export.hpp"
#endif

#include "libtorrent/aux_/disable_warnings_pop.hpp"

#include "libtorrent/random.hpp"
#include "libtorrent/alloca.hpp"
#include "libtorrent/pe_crypto.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/span.hpp"

namespace libtorrent
{
	namespace mp = boost::multiprecision;

	namespace {
		// TODO: it would be nice to get the literal working
		key_t const dh_prime
			("0xFFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A63A36210000000000090563");
	}

	// Set the prime P and the generator, generate local public key
	dh_key_exchange::dh_key_exchange()
	{
		std::array<std::uint8_t, 96> random_key;
		for (auto& i : random_key) i = random();

		// create local key (random)
		mp::import_bits(m_dh_local_secret, random_key.begin(), random_key.end());

		// key = (2 ^ secret) % prime
		m_dh_local_key = mp::powm(key_t(2), m_dh_local_secret, dh_prime);
	}

	// compute shared secret given remote public key
	void dh_key_exchange::compute_secret(std::uint8_t const* remote_pubkey)
	{
		TORRENT_ASSERT(remote_pubkey);
		key_t key;
		mp::import_bits(key, remote_pubkey, remote_pubkey + 96);
		compute_secret(key);
	}

	void dh_key_exchange::compute_secret(key_t const& remote_pubkey)
	{
		// shared_secret = (remote_pubkey ^ local_secret) % prime
		m_dh_shared_secret = mp::powm(remote_pubkey, m_dh_local_secret, dh_prime);

		std::array<char, 96> buffer;
		mp::export_bits(m_dh_shared_secret, reinterpret_cast<std::uint8_t*>(buffer.data()), 8);

		static char const req3[4] = {'r', 'e', 'q', '3'};
		// calculate the xor mask for the obfuscated hash
		m_xor_mask = hasher(req3).update(buffer).final();
	}

	std::tuple<int, span<span<char const>>>
	encryption_handler::encrypt(
		span<span<char>> iovec)
	{
		TORRENT_ASSERT(!m_send_barriers.empty());
		TORRENT_ASSERT(m_send_barriers.front().enc_handler);

		int to_process = m_send_barriers.front().next;

		span<char>* bufs;
		size_t num_bufs;
		bool need_destruct = false;
		if (to_process != INT_MAX)
		{
			bufs = TORRENT_ALLOCA(span<char>, iovec.size());
			need_destruct = true;
			num_bufs = 0;
			for (int i = 0; to_process > 0 && i < iovec.size(); ++i)
			{
				++num_bufs;
				int const size = int(iovec[i].size());
				if (to_process < size)
				{
					new (&bufs[i]) span<char>(
						iovec[i].data(), to_process);
					to_process = 0;
				}
				else
				{
					new (&bufs[i]) span<char>(iovec[i]);
					to_process -= size;
				}
			}
		}
		else
		{
			bufs = iovec.data();
			num_bufs = iovec.size();
		}

		int next_barrier = 0;
		span<span<char const>> out_iovec;
		if (num_bufs != 0)
		{
			std::tie(next_barrier, out_iovec)
				= m_send_barriers.front().enc_handler->encrypt({bufs, size_t(num_bufs)});
		}

		if (m_send_barriers.front().next != INT_MAX)
		{
			// to_process holds the difference between the size of the buffers
			// and the bytes left to the next barrier
			// if it's zero then pop the barrier
			// otherwise update the number of bytes remaining to the next barrier
			if (to_process == 0)
			{
				if (m_send_barriers.size() == 1)
				{
					// transitioning back to plaintext
					next_barrier = INT_MAX;
				}
				m_send_barriers.pop_front();
			}
			else
			{
				m_send_barriers.front().next = to_process;
			}
		}

#if TORRENT_USE_ASSERTS
		if (next_barrier != INT_MAX && next_barrier != 0)
		{
			int payload = 0;
			for (int i = 0; i < num_bufs; ++i)
				payload += int(bufs[i].size());

			int overhead = 0;
			for (auto buf : out_iovec)
				overhead += int(buf.size());
			TORRENT_ASSERT(overhead + payload == next_barrier);
		}
#endif
		if (need_destruct)
		{
			for (int i = 0; i < num_bufs; ++i)
				bufs[i].~span<char>();
		}
		return std::make_tuple(next_barrier, out_iovec);
	}

	int encryption_handler::decrypt(crypto_receive_buffer& recv_buffer
		, std::size_t& bytes_transferred)
	{
		TORRENT_ASSERT(!is_recv_plaintext());
		int consume = 0;
		if (recv_buffer.crypto_packet_finished())
		{
			span<char> wr_buf = recv_buffer.mutable_buffer(bytes_transferred);
			int packet_size = 0;
			int produce = int(bytes_transferred);
			m_dec_handler->decrypt(wr_buf, consume, produce, packet_size);
			TORRENT_ASSERT(packet_size || produce);
			TORRENT_ASSERT(packet_size >= 0);
			bytes_transferred = produce;
			if (packet_size)
				recv_buffer.crypto_cut(consume, packet_size);
		}
		else
			bytes_transferred = 0;
		return consume;
	}

	bool encryption_handler::switch_send_crypto(boost::shared_ptr<crypto_plugin> crypto
		, int pending_encryption)
	{
		bool place_barrier = false;
		if (!m_send_barriers.empty())
		{
			std::list<barrier>::iterator end = m_send_barriers.end(); --end;
			for (std::list<barrier>::iterator b = m_send_barriers.begin();
				b != end; ++b)
				pending_encryption -= b->next;
			TORRENT_ASSERT(pending_encryption >= 0);
			m_send_barriers.back().next = pending_encryption;
		}
		else if (crypto)
			place_barrier = true;

		if (crypto)
			m_send_barriers.push_back(barrier(crypto, INT_MAX));

		return place_barrier;
	}

	void encryption_handler::switch_recv_crypto(boost::shared_ptr<crypto_plugin> crypto
		, crypto_receive_buffer& recv_buffer)
	{
		m_dec_handler = crypto;
		int packet_size = 0;
		if (crypto)
		{
			int consume = 0;
			int produce = 0;
			std::vector<span<char>> wr_buf;
			crypto->decrypt(wr_buf, consume, produce, packet_size);
			TORRENT_ASSERT(wr_buf.empty());
			TORRENT_ASSERT(consume == 0);
			TORRENT_ASSERT(produce == 0);
		}
		recv_buffer.crypto_reset(packet_size);
	}

	rc4_handler::rc4_handler()
		: m_encrypt(false)
		, m_decrypt(false)
	{
		m_rc4_incoming.x = 0;
		m_rc4_incoming.y = 0;
		m_rc4_outgoing.x = 0;
		m_rc4_outgoing.y = 0;
	}

	void rc4_handler::set_incoming_key(unsigned char const* key, int len)
	{
		m_decrypt = true;
		rc4_init(key, len, &m_rc4_incoming);
		// Discard first 1024 bytes
		int consume = 0;
		int produce = 0;
		int packet_size = 0;
		char buf[1024];
		span<char> vec(buf, sizeof(buf));
		decrypt(vec, consume, produce, packet_size);
	}

	void rc4_handler::set_outgoing_key(unsigned char const* key, int len)
	{
		m_encrypt = true;
		rc4_init(key, len, &m_rc4_outgoing);
		// Discard first 1024 bytes
		char buf[1024];
		span<char> vec(buf, sizeof(buf));
		encrypt(vec);
	}

	std::tuple<int, span<span<char const>>>
	rc4_handler::encrypt(span<span<char>> bufs)
	{
		span<span<char const>> empty;
		if (!m_encrypt) return std::make_tuple(0, empty);
		if (bufs.size() == 0) return std::make_tuple(0, empty);

		int bytes_processed = 0;
		for (auto& buf : bufs)
		{
			unsigned char* const pos = reinterpret_cast<unsigned char*>(buf.data());
			int const len = int(buf.size());

			TORRENT_ASSERT(len >= 0);
			TORRENT_ASSERT(pos);

			bytes_processed += len;
			rc4_encrypt(pos, len, &m_rc4_outgoing);
		}
		return std::make_tuple(bytes_processed, empty);
	}

	void rc4_handler::decrypt(span<span<char>> bufs
		, int& consume
		, int& produce
		, int& packet_size)
	{
		// these are out-parameters that are not set
		TORRENT_UNUSED(consume);
		TORRENT_UNUSED(packet_size);

		if (!m_decrypt) return;

		int bytes_processed = 0;
		for (auto& buf : bufs)
		{
			unsigned char* const pos = reinterpret_cast<unsigned char*>(buf.data());
			int const len = int(buf.size());

			TORRENT_ASSERT(len >= 0);
			TORRENT_ASSERT(pos);

			bytes_processed += len;
			rc4_encrypt(pos, len, &m_rc4_incoming);
		}
		produce = bytes_processed;
	}

// All this code is based on libTomCrypt (http://www.libtomcrypt.com/)
// this library is public domain and has been specially
// tailored for libtorrent by Arvid Norberg

void rc4_init(const unsigned char* in, unsigned long len, rc4 *state)
{
	size_t const key_size = sizeof(state->buf);
	unsigned char key[key_size], tmp, *s;
	int keylen, x, y, j;

	TORRENT_ASSERT(state != nullptr);
	TORRENT_ASSERT(len <= key_size);
	if (len > key_size) len = key_size;

	state->x = 0;
	while (len--) {
		state->buf[state->x++] = *in++;
	}

	/* extract the key */
	s = state->buf.data();
	std::memcpy(key, s, key_size);
	keylen = state->x;

	/* make RC4 perm and shuffle */
	for (x = 0; x < key_size; ++x) {
		s[x] = x;
	}

	for (j = x = y = 0; x < key_size; x++) {
		y = (y + state->buf[x] + key[j++]) & 255;
		if (j == keylen) {
			j = 0;
		}
		tmp = s[x]; s[x] = s[y]; s[y] = tmp;
	}
	state->x = 0;
	state->y = 0;
}

unsigned long rc4_encrypt(unsigned char *out, unsigned long outlen, rc4 *state)
{
	unsigned char x, y, *s, tmp;
	unsigned long n;

	TORRENT_ASSERT(out != nullptr);
	TORRENT_ASSERT(state != nullptr);

	n = outlen;
	x = state->x;
	y = state->y;
	s = state->buf.data();
	while (outlen--) {
		x = (x + 1) & 255;
		y = (y + s[x]) & 255;
		tmp = s[x]; s[x] = s[y]; s[y] = tmp;
		tmp = (s[x] + s[y]) & 255;
		*out++ ^= s[tmp];
	}
	state->x = x;
	state->y = y;
	return n;
}

} // namespace libtorrent

#endif // #if !defined(TORRENT_DISABLE_ENCRYPTION) && !defined(TORRENT_DISABLE_EXTENSIONS)
