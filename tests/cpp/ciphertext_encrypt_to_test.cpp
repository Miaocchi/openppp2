#include <ppp/cryptography/Ciphertext.h>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// EncryptTo() writes into caller memory; it must produce exactly the bytes Encrypt()
// returns, because the transmission frame path switched to it without a wire change.
void CheckMethod(const char* method) {
    if (!ppp::cryptography::Ciphertext::Support(method)) {
        std::cout << "skip: " << method << " not supported here" << std::endl;
        return;
    }

    for (int length : { 1, 15, 16, 17, 1400, 65535 }) {
        std::vector<ppp::Byte> plaintext(length);
        for (int i = 0; i < length; i++) {
            plaintext[i] = static_cast<ppp::Byte>(i * 31 + 7);
        }

        // Fresh instances per call so stateful backends start from the same state.
        auto reference = std::make_shared<ppp::cryptography::Ciphertext>(method, "encrypt-to-test");
        auto candidate = std::make_shared<ppp::cryptography::Ciphertext>(method, "encrypt-to-test");

        int expected_length = 0;
        std::shared_ptr<ppp::Byte> expected = reference->Encrypt(nullptr, plaintext.data(), length, expected_length);
        Require(expected != nullptr && expected_length == length, std::string(method) + ": Encrypt failed");

        std::vector<ppp::Byte> output(length + ppp::cryptography::Ciphertext::EncryptToSlack + 8, 0xA5);
        int actual_length = 0;
        Require(candidate->EncryptTo(output.data() + 8, static_cast<int>(output.size()) - 8, plaintext.data(), length, actual_length),
            std::string(method) + ": EncryptTo failed");
        Require(actual_length == expected_length, std::string(method) + ": EncryptTo length differs");
        Require(std::memcmp(output.data() + 8, expected.get(), length) == 0, std::string(method) + ": EncryptTo bytes differ");
        for (int i = 0; i < 8; i++) {
            Require(output[i] == 0xA5, std::string(method) + ": EncryptTo wrote before the output pointer");
        }

        auto decoder = std::make_shared<ppp::cryptography::Ciphertext>(method, "encrypt-to-test");
        int decoded_length = 0;
        std::shared_ptr<ppp::Byte> decoded = decoder->Decrypt(nullptr, output.data() + 8, actual_length, decoded_length);
        Require(decoded != nullptr && decoded_length == length && std::memcmp(decoded.get(), plaintext.data(), length) == 0,
            std::string(method) + ": EncryptTo output does not decrypt");
    }

    ppp::Byte input[4] = { 1, 2, 3, 4 };
    ppp::Byte small[2] = {};
    int ignored = 0;
    auto cipher = std::make_shared<ppp::cryptography::Ciphertext>(method, "encrypt-to-test");
    Require(!cipher->EncryptTo(small, sizeof(small), input, sizeof(input), ignored),
        std::string(method) + ": EncryptTo accepted an undersized output");

    std::cout << "ok: " << method << std::endl;
}

} // namespace

int main() {
    try {
        for (const char* method : { "aes-128-cfb", "aes-256-cfb", "simd-aes-128-cfb", "simd-aes-256-cfb",
                 "aes-128-ctr", "aes-256-gcm", "chacha20", "rc4-md5", "rc4-sha256" }) {
            CheckMethod(method);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "PASS: ciphertext_encrypt_to_test" << std::endl;
    return 0;
}
