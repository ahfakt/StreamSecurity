#include "StreamSecurity/TLS.h"
#include <openssl/err.h>

#define ExpectInitialized(x) if (!x) throw Exception(static_cast<TLS::Exception::Code>(ERR_peek_last_error()))
#define Expect1(x) if (1 != x) throw Exception(static_cast<Exception::Code>(ERR_peek_last_error()))
#define ExpectPos(x) if (0 >= x) throw Exception(static_cast<Exception::Code>(ERR_peek_last_error()))
#define ExpectNNeg(x) if (0 > x) throw Exception(static_cast<Exception::Code>(ERR_peek_last_error()))
#define MAX_TLS_RECORD_SIZE 16*1024

namespace Stream::Security {

TLSDecrypt::TLSDecrypt(SSL* ssl)
		: mSSL(ssl)
{
	if (ssl) {
		mInBio = BIO_new(BIO_s_mem());
		ExpectInitialized(mInBio);
		BIO_set_mem_eof_return(mInBio, -1);
		SSL_set0_rbio(ssl, mInBio);
	}
}

void
swap(TLSDecrypt& a, TLSDecrypt& b) noexcept
{
	swap(static_cast<BufferInput&>(a), static_cast<BufferInput&>(b));
	std::swap(a.mSSL, b.mSSL);
	std::swap(a.mInBio, b.mInBio);
}

TLSDecrypt&
TLSDecrypt::operator=(TLSDecrypt&& other) noexcept
{
	static_cast<BufferInput&>(*this) = static_cast<BufferInput&&>(other);
	std::swap(mSSL, other.mSSL);
	std::swap(mInBio, other.mInBio);
	return *this;
}

void
TLSDecrypt::recvData()
{
	std::size_t received = provideData(MAX_TLS_RECORD_SIZE);
	int r = BIO_write_ex(mInBio, mGet, received, &received);
	if (r == 1)
		mGet += received;
	else
		throw Exception(static_cast<TLS::Exception::Code>(ERR_peek_last_error()));
}

std::size_t
TLSDecrypt::readBytes(std::byte* dest, std::size_t size)
{
	std::size_t outl = 0;
	int r = SSL_is_init_finished(mSSL)
			? SSL_read_ex(mSSL, dest, size, &outl)
			: SSL_do_handshake(mSSL);

	if (r == 1)
		return outl;

	r = SSL_get_error(mSSL, r);
	switch (r) {
		case SSL_ERROR_WANT_READ: {
			wantSendData();
			recvData();
			return 0;
		}
		default:
			throw Exception(static_cast<TLS::Exception::Code>(ERR_peek_last_error()));
	}
}

TLSEncrypt::TLSEncrypt(SSL* ssl)
		: mSSL(ssl)
{
	if (ssl) {
		mOutBio = BIO_new(BIO_s_mem());
		ExpectInitialized(mOutBio);
		BIO_set_mem_eof_return(mOutBio, -1);
		SSL_set0_wbio(ssl, mOutBio);
	}
}

void
swap(TLSEncrypt& a, TLSEncrypt& b) noexcept
{
	swap(static_cast<BufferOutput&>(a), static_cast<BufferOutput&>(b));
	std::swap(a.mSSL, b.mSSL);
	std::swap(a.mOutBio, b.mOutBio);
}

TLSEncrypt&
TLSEncrypt::operator=(TLSEncrypt&& other) noexcept
{
	static_cast<BufferOutput&>(*this) = static_cast<BufferOutput&&>(other);
	std::swap(mSSL, other.mSSL);
	std::swap(mOutBio, other.mOutBio);
	return *this;
}

void
TLSEncrypt::sendData()
{
	std::size_t pending = BIO_ctrl_pending(mOutBio);
	if (pending > 0) {
		provideSpace(pending);
		int r = BIO_read_ex(mOutBio, mPut, pending, &pending);
		if (r == 1)
			mPut += pending;
		else
			throw Exception(static_cast<TLS::Exception::Code>(ERR_peek_last_error()));
	}
}

std::size_t
TLSEncrypt::writeBytes(std::byte const* src, std::size_t size)
{
	std::size_t inl = 0;
	int r = SSL_is_init_finished(mSSL)
			? SSL_write_ex(mSSL, src, size, &inl)
			: SSL_do_handshake(mSSL);

	if (r == 1) {
		if (inl)
			sendData();
		return inl;
	}

	r = SSL_get_error(mSSL, r);
	switch (r) {
		case SSL_ERROR_WANT_READ: {
			sendData();
			flush();
			wantRecvData();
			return 0;
		}
		default:
			throw Exception(static_cast<TLS::Exception::Code>(ERR_peek_last_error()));
	}
}

bool
TLSEncrypt::shutdown()
{
	int r = SSL_shutdown(mSSL);

	if (r == 1)
		return true;

	if (r == 0) {
		sendData();
		flush();
		try {
			wantRecvData();
		} catch (Input::Exception& exc) {
			return false;
		}
		return shutdown();
	}

	throw Exception(static_cast<TLS::Exception::Code>(ERR_peek_last_error()));
}

TLS::TLS(SSL* ssl)
		: TLSDecrypt(ssl)
		, TLSEncrypt(ssl)
		, mSSL(ssl, SSL_free)
{ ExpectInitialized(mSSL); }

TLS::TLS(Context const& ctx)
		: TLS(SSL_new(ctx.mCtx.get()))
{
	if (SSL_is_server(mSSL.get()))
		SSL_set_accept_state(mSSL.get());
	else
		SSL_set_connect_state(mSSL.get());
}

void
swap(TLS& a, TLS& b) noexcept
{
	swap(static_cast<TLSDecrypt&>(a), static_cast<TLSDecrypt&>(b));
	swap(static_cast<TLSEncrypt&>(a), static_cast<TLSEncrypt&>(b));
	std::swap(a.mSSL, b.mSSL);
}

TLS&
TLS::operator=(TLS&& other) noexcept
{
	static_cast<TLSDecrypt&>(*this) = static_cast<TLSDecrypt&&>(other);
	static_cast<TLSEncrypt&>(*this) = static_cast<TLSEncrypt&&>(other);
	std::swap(mSSL, other.mSSL);
	return *this;
}

void
TLS::wantSendData()
{
	sendData();
	flush();
}

void
TLS::wantRecvData()
{ recvData(); }

TLS::Context::Context(SSL_METHOD const* method)
		: mCtx(SSL_CTX_new(method), SSL_CTX_free)
{ ExpectInitialized(mCtx); }

TLS::Context::Context(SSL_METHOD const* method, Certificate const& certificate, PrivateKey const& privateKey)
		: Context(method)
{
	Expect1(SSL_CTX_use_PrivateKey(mCtx.get(), static_cast<EVP_PKEY*>(Stream::Security::Key(privateKey))));
	Expect1(SSL_CTX_use_certificate(mCtx.get(), static_cast<X509*>(certificate)));
	Expect1(SSL_CTX_check_private_key(mCtx.get()));
}

void
TLS::Context::addToStore(Certificate const& caCertificate)
{ Expect1(X509_STORE_add_cert(SSL_CTX_get_cert_store(mCtx.get()), static_cast<X509*>(caCertificate))); }

std::error_code
make_error_code(TLS::Exception::Code e) noexcept
{
	static struct : std::error_category {
		[[nodiscard]] char const*
		name() const noexcept override
		{ return "Stream::Security::TLS"; }

		[[nodiscard]] std::string
		message(int ev) const noexcept override
		{ return ERR_error_string(ev, nullptr); }
	} instance;
	return {static_cast<int>(e), instance};
}

}//namespace Stream::Security
