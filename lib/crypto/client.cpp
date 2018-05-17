#include <iostream>

#include "mtxclient/crypto/client.hpp"
#include <olm/base64.hh>

using json = nlohmann::json;
using namespace mtx::crypto;

void
OlmClient::create_new_account()
{
        // The method has no effect if the account is already initialized.
        if (account_)
                return;

        account_ = create_olm_object<OlmAccount>();

        auto tmp_buf  = create_buffer(olm_create_account_random_length(account_.get()));
        const int ret = olm_create_account(account_.get(), tmp_buf.data(), tmp_buf.size());

        if (ret == -1) {
                account_.reset();
                throw olm_exception("create_new_account", account_.get());
        }
}

void
OlmClient::create_new_utility()
{
        // The method has no effect if the account is already initialized.
        if (utility_)
                return;

        utility_ = create_olm_object<OlmUtility>();
}

IdentityKeys
OlmClient::identity_keys()
{
        auto tmp_buf = create_buffer(olm_account_identity_keys_length(account_.get()));
        int result =
          olm_account_identity_keys(account_.get(), (void *)tmp_buf.data(), tmp_buf.size());

        if (result == -1)
                throw olm_exception("identity_keys", account_.get());

        return json::parse(std::string(tmp_buf.begin(), tmp_buf.end()));
}

std::string
OlmClient::sign_message(const std::string &msg)
{
        auto signature_buf = create_buffer(olm_account_signature_length(account_.get()));
        olm_account_sign(
          account_.get(), msg.data(), msg.size(), signature_buf.data(), signature_buf.size());

        return std::string(signature_buf.begin(), signature_buf.end());
}

std::string
OlmClient::sign_identity_keys()
{
        auto keys = identity_keys();

        json body{{"algorithms", {"m.olm.v1.curve25519-aes-sha2", "m.megolm.v1.aes-sha2"}},
                  {"user_id", user_id_},
                  {"device_id", device_id_},
                  {"keys",
                   {
                     {"curve25519:" + device_id_, keys.curve25519},
                     {"ed25519:" + device_id_, keys.ed25519},
                   }}};

        return sign_message(body.dump());
}

std::size_t
OlmClient::generate_one_time_keys(std::size_t number_of_keys)
{
        const std::size_t nbytes =
          olm_account_generate_one_time_keys_random_length(account_.get(), number_of_keys);

        auto buf = create_buffer(nbytes);

        const int ret = olm_account_generate_one_time_keys(
          account_.get(), number_of_keys, buf.data(), buf.size());

        if (ret == -1)
                throw olm_exception("generate_one_time_keys", account_.get());

        return ret;
}

OneTimeKeys
OlmClient::one_time_keys()
{
        auto buf = create_buffer(olm_account_one_time_keys_length(account_.get()));

        const int ret = olm_account_one_time_keys(account_.get(), buf.data(), buf.size());

        if (ret == -1)
                throw olm_exception("one_time_keys", account_.get());

        return json::parse(std::string(buf.begin(), buf.end()));
}

std::string
OlmClient::sign_one_time_key(const std::string &key)
{
        json j{{"key", key}};
        return sign_message(j.dump());
}

std::map<std::string, json>
OlmClient::sign_one_time_keys(const OneTimeKeys &keys)
{
        // Sign & append the one time keys.
        std::map<std::string, json> signed_one_time_keys;
        for (const auto &elem : keys.curve25519) {
                auto sig = sign_one_time_key(elem.second);

                signed_one_time_keys["signed_curve25519:" + elem.first] =
                  signed_one_time_key_json(elem.second, sig);
        }

        return signed_one_time_keys;
}

json
OlmClient::signed_one_time_key_json(const std::string &key, const std::string &signature)
{
        return json{{"key", key},
                    {"signatures", {{user_id_, {{"ed25519:" + device_id_, signature}}}}}};
}

mtx::requests::UploadKeys
OlmClient::create_upload_keys_request()
{
        return create_upload_keys_request(one_time_keys());
}

mtx::requests::UploadKeys
OlmClient::create_upload_keys_request(const mtx::crypto::OneTimeKeys &one_time_keys)
{
        mtx::requests::UploadKeys req;
        req.device_keys.user_id   = user_id_;
        req.device_keys.device_id = device_id_;

        auto id_keys = identity_keys();

        req.device_keys.keys["curve25519:" + device_id_] = id_keys.curve25519;
        req.device_keys.keys["ed25519:" + device_id_]    = id_keys.ed25519;

        // Generate and add the signature to the request.
        auto sig = sign_identity_keys();

        req.device_keys.signatures[user_id_]["ed25519:" + device_id_] = sig;

        if (one_time_keys.curve25519.empty())
                return req;

        // Sign & append the one time keys.
        req.one_time_keys = sign_one_time_keys(one_time_keys);

        return req;
}

std::unique_ptr<OlmOutboundGroupSession, OlmDeleter>
OlmClient::init_outbound_group_session()
{
        auto session = create_olm_object<OlmOutboundGroupSession>();
        auto tmp_buf = create_buffer(olm_init_outbound_group_session_random_length(session.get()));

        const int ret =
          olm_init_outbound_group_session(session.get(), tmp_buf.data(), tmp_buf.size());

        if (ret == -1)
                throw olm_exception("init_outbound_group_session", session.get());

        return session;
}

BinaryBuf
OlmClient::decrypt_message(OlmSession *session,
                           size_t msgtype,
                           const std::string &one_time_key_message)
{
        auto tmp = create_buffer(one_time_key_message.size());
        std::copy(one_time_key_message.begin(), one_time_key_message.end(), tmp.begin());

        auto declen =
          olm_decrypt_max_plaintext_length(session, msgtype, (void *)tmp.data(), tmp.size());

        auto decrypted = create_buffer(declen);
        std::copy(one_time_key_message.begin(), one_time_key_message.end(), tmp.begin());

        const int nbytes = olm_decrypt(
          session, msgtype, (void *)tmp.data(), tmp.size(), decrypted.data(), decrypted.size());

        if (nbytes == -1)
                throw olm_exception("olm_decrypt", session);

        // Removing the extra padding from the origial buffer.
        auto output = create_buffer(nbytes);
        std::memcpy(output.data(), decrypted.data(), nbytes);

        return output;
}

BinaryBuf
OlmClient::encrypt_message(OlmSession *session, const std::string &msg)
{
        auto ciphertext = create_buffer(olm_encrypt_message_length(session, msg.size()));
        auto random_buf = create_buffer(olm_encrypt_random_length(session));

        const int ret = olm_encrypt(session,
                                    msg.data(),
                                    msg.size(),
                                    random_buf.data(),
                                    random_buf.size(),
                                    ciphertext.data(),
                                    ciphertext.size());
        if (ret == -1)
                throw olm_exception("olm_encrypt", session);

        return ciphertext;
}

std::unique_ptr<OlmSession, OlmDeleter>
OlmClient::create_inbound_session(const BinaryBuf &one_time_key_message)
{
        auto session = create_olm_object<OlmSession>();

        auto tmp = create_buffer(one_time_key_message.size());
        std::copy(one_time_key_message.begin(), one_time_key_message.end(), tmp.begin());

        const int ret =
          olm_create_inbound_session(session.get(), account(), (void *)tmp.data(), tmp.size());

        if (ret == -1)
                throw olm_exception("create_inbound_session", session.get());

        return session;
}

std::unique_ptr<OlmSession, OlmDeleter>
OlmClient::create_outbound_session(const std::string &identity_key, const std::string &one_time_key)
{
        auto session    = create_olm_object<OlmSession>();
        auto random_buf = create_buffer(olm_create_outbound_session_random_length(session.get()));

        const int ret = olm_create_outbound_session(session.get(),
                                                    account(),
                                                    identity_key.data(),
                                                    identity_key.size(),
                                                    one_time_key.data(),
                                                    one_time_key.size(),
                                                    random_buf.data(),
                                                    random_buf.size());

        if (ret == -1)
                throw olm_exception("create_outbound_session", session.get());

        return session;
}

BinaryBuf
mtx::crypto::decode_base64(const std::string &msg)
{
        const int output_nbytes = olm::decode_base64_length(msg.size());

        if (output_nbytes == -1)
                throw std::runtime_error("invalid base64 input length");

        auto output_buf = create_buffer(output_nbytes);

        olm::decode_base64(
          reinterpret_cast<const uint8_t *>(msg.data()), msg.size(), output_buf.data());

        return output_buf;
}

std::string
mtx::crypto::encode_base64(const uint8_t *data, std::size_t len)
{
        const int output_nbytes = olm::encode_base64_length(len);

        if (output_nbytes == -1)
                throw std::runtime_error("invalid base64 input length");

        auto output_buf = create_buffer(output_nbytes);
        olm::encode_base64(data, len, output_buf.data());

        return std::string(output_buf.begin(), output_buf.end());
}

std::string
mtx::crypto::session_id(OlmOutboundGroupSession *s)
{
        auto tmp = create_buffer(olm_outbound_group_session_id_length(s));
        olm_outbound_group_session_id(s, tmp.data(), tmp.size());

        return std::string(tmp.begin(), tmp.end());
}

std::string
mtx::crypto::session_key(OlmOutboundGroupSession *s)
{
        auto tmp = create_buffer(olm_outbound_group_session_key_length(s));
        olm_outbound_group_session_key(s, tmp.data(), tmp.size());

        return std::string(tmp.begin(), tmp.end());
}

bool
mtx::crypto::matches_inbound_session(OlmSession *session, const BinaryBuf &one_time_key_message)
{
        auto tmp = create_buffer(one_time_key_message.size());
        std::copy(one_time_key_message.begin(), one_time_key_message.end(), tmp.begin());

        return olm_matches_inbound_session(session, (void *)tmp.data(), tmp.size());
}

bool
mtx::crypto::matches_inbound_session_from(OlmSession *session,
                                          const std::string &id_key,
                                          const BinaryBuf &one_time_key_message)
{
        auto tmp = create_buffer(one_time_key_message.size());
        std::copy(one_time_key_message.begin(), one_time_key_message.end(), tmp.begin());

        return olm_matches_inbound_session_from(
          session, id_key.data(), id_key.size(), (void *)tmp.data(), tmp.size());
}