#include <benchmark/benchmark.h>

#include <ppp/cryptography/Ciphertext.h>
#include <ppp/cryptography/EVP.h>
#include <ppp/cryptography/ssea.h>
#include <ppp/cryptography/ssea_simd/sse_dispatch.h>

#include <array>
#include <cstring>
#include <vector>

using ppp::Byte;
using ppp::cryptography::Ciphertext;
using ppp::cryptography::EVP;
using ppp::cryptography::ssea;

namespace {
int delta_encode_inplace(Byte* data, int data_size, int kf) {
    if (::ssea::SimdLevel::AVX2 == ::ssea::ssea_cpu_level()) {
        const int encoded = ::ssea::delta_encode_inplace_avx2(data, data_size, kf);
        if (encoded > 0) return encoded;
    }
    return ::ssea::delta_encode_inplace_scalar(data, data_size, kf);
}

struct Chain {
    std::shared_ptr<Ciphertext> protocol;
    std::shared_ptr<Ciphertext> transport;

    explicit Chain(const char* method) {
        EVP::SetSimdAuto(false);
        protocol = std::make_shared<Ciphertext>(ppp::string(method), ppp::string("protocol-key"));
        transport = std::make_shared<Ciphertext>(ppp::string(method), ppp::string("transport-key"));
        EVP::SetSimdAuto(true);
    }

    std::shared_ptr<Byte> encrypt(Byte* input, int size, int& output_size) {
        constexpr int header_size = 3;
        const int capacity = header_size + size + Ciphertext::EncryptToSlack;
        auto output = ppp::threading::BufferswapAllocator::MakeByteArray(nullptr, capacity);
        if (!output) return nullptr;

        Byte* payload = output.get() + header_size;
        int payload_size = 0;
        if (!transport->EncryptTo(payload, capacity - header_size, input, size, payload_size) ||
            payload_size != size) return nullptr;

        const int kf = 0x4f13 ^ 0x5a;
        ppp::cryptography::ssea::masked_xor_random_next(payload, payload + payload_size, kf);
        ppp::cryptography::ssea::shuffle_data(reinterpret_cast<char*>(payload), payload_size, kf);
        if (delta_encode_inplace(payload, payload_size, 0x4f13) != payload_size) return nullptr;

        std::array<Byte, header_size> header{{0x5a, static_cast<Byte>((size - 1) >> 8), static_cast<Byte>((size - 1) & 0xff)}};
        std::array<Byte, 2 + Ciphertext::EncryptToSlack> encrypted_header;
        int encrypted_header_size = 0;
        if (!protocol->EncryptTo(encrypted_header.data(), static_cast<int>(encrypted_header.size()),
            header.data() + 1, 2, encrypted_header_size) || encrypted_header_size != 2) return nullptr;
        std::memcpy(header.data() + 1, encrypted_header.data(), 2);
        header[1] ^= static_cast<Byte>(kf);
        header[2] ^= static_cast<Byte>(kf);
        ppp::cryptography::ssea::shuffle_data(reinterpret_cast<char*>(header.data() + 1), 2, kf);
        if (delta_encode_inplace(header.data(), header_size, 0x4f13) != header_size) return nullptr;

        std::memcpy(output.get(), header.data(), header_size);
        output_size = header_size + payload_size;
        return output;
    }

    bool encrypt_many(Byte* input, int size, int count, std::vector<std::shared_ptr<Byte>>& outputs) {
        if (count < 1) return false;

        constexpr int header_size = 3;
        const int capacity = header_size + size + Ciphertext::EncryptToSlack;
        outputs.resize(static_cast<size_t>(count));
        std::vector<Byte*> payloads(static_cast<size_t>(count));
        std::vector<Byte*> inputs(static_cast<size_t>(count));
        std::vector<int> capacities(static_cast<size_t>(count));
        std::vector<int> lengths(static_cast<size_t>(count));
        std::vector<int> outlens(static_cast<size_t>(count));

        for (int i = 0; i < count; i++) {
            outputs[(size_t)i] = ppp::threading::BufferswapAllocator::MakeByteArray(nullptr, capacity);
            if (!outputs[(size_t)i]) return false;
            payloads[(size_t)i] = outputs[(size_t)i].get() + header_size;
            inputs[(size_t)i] = input;
            capacities[(size_t)i] = capacity - header_size;
            lengths[(size_t)i] = size;
        }

        if (!transport->EncryptManyTo(payloads.data(), capacities.data(), inputs.data(),
            lengths.data(), count, outlens.data())) return false;

        const int kf = 0x4f13 ^ 0x5a;
        for (int i = 0; i < count; i++) {
            if (outlens[(size_t)i] != size) return false;
            Byte* payload = payloads[(size_t)i];
            ppp::cryptography::ssea::masked_xor_random_next(payload, payload + size, kf);
            ppp::cryptography::ssea::shuffle_data(reinterpret_cast<char*>(payload), size, kf);
            if (delta_encode_inplace(payload, size, 0x4f13) != size) return false;

            std::array<Byte, header_size> header{{0x5a, static_cast<Byte>((size - 1) >> 8), static_cast<Byte>((size - 1) & 0xff)}};
            std::array<Byte, 2 + Ciphertext::EncryptToSlack> encrypted_header;
            int encrypted_header_size = 0;
            if (!protocol->EncryptTo(encrypted_header.data(), static_cast<int>(encrypted_header.size()),
                header.data() + 1, 2, encrypted_header_size) || encrypted_header_size != 2) return false;
            std::memcpy(header.data() + 1, encrypted_header.data(), 2);
            header[1] ^= static_cast<Byte>(kf);
            header[2] ^= static_cast<Byte>(kf);
            ppp::cryptography::ssea::shuffle_data(reinterpret_cast<char*>(header.data() + 1), 2, kf);
            if (delta_encode_inplace(header.data(), header_size, 0x4f13) != header_size) return false;
            std::memcpy(outputs[(size_t)i].get(), header.data(), header_size);
        }
        return true;
    }
};

void BM_CryptoChain(benchmark::State& state, const char* method) {
    if (!Ciphertext::Support(ppp::string(method))) {
        state.SkipWithError("cipher method not supported");
        return;
    }
    Chain chain(method);
    std::vector<Byte> payload(static_cast<size_t>(state.range(0)), 0x2a);
    int check_size = 0;
    if (!chain.encrypt(payload.data(), static_cast<int>(payload.size()), check_size) ||
        check_size != static_cast<int>(payload.size()) + 3) {
        state.SkipWithError("full crypto-chain self-check failed");
        return;
    }
    for (auto _ : state) {
        int output_size = 0;
        auto output = chain.encrypt(payload.data(), static_cast<int>(payload.size()), output_size);
        benchmark::DoNotOptimize(output.get());
        benchmark::DoNotOptimize(output_size);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(payload.size()));
    state.counters["allocations"] = 1;
}

static bool api_batch_matches_single(Chain& chain, int size, int count) {
    const int capacity = size + Ciphertext::EncryptToSlack;
    std::vector<Byte> input(static_cast<size_t>(size), 0x2a);
    std::vector<std::vector<Byte>> batched(static_cast<size_t>(count),
        std::vector<Byte>(static_cast<size_t>(capacity)));
    std::vector<std::vector<Byte>> single(static_cast<size_t>(count),
        std::vector<Byte>(static_cast<size_t>(capacity)));
    std::vector<Byte*> batch_outputs;
    std::vector<Byte*> inputs;
    std::vector<int> capacities;
    std::vector<int> lengths;
    std::vector<int> outlens;
    std::vector<int> single_lengths(static_cast<size_t>(count));
    batch_outputs.reserve(static_cast<size_t>(count));
    inputs.reserve(static_cast<size_t>(count));
    capacities.reserve(static_cast<size_t>(count));
    lengths.reserve(static_cast<size_t>(count));
    outlens.resize(static_cast<size_t>(count));

    for (int i = 0; i < count; i++) {
        batch_outputs.push_back(batched[(size_t)i].data());
        inputs.push_back(input.data());
        capacities.push_back(capacity);
        lengths.push_back(size);
    }

    if (!chain.transport->EncryptManyTo(batch_outputs.data(), capacities.data(), inputs.data(),
        lengths.data(), count, outlens.data())) return false;

    for (int i = 0; i < count; i++) {
        if (!chain.transport->EncryptTo(single[(size_t)i].data(), capacity, input.data(), size,
            single_lengths[(size_t)i])) return false;
        if (outlens[(size_t)i] != single_lengths[(size_t)i]) return false;
        if (std::memcmp(batched[(size_t)i].data(), single[(size_t)i].data(),
            static_cast<size_t>(size)) != 0) return false;
    }
    return true;
}

void BM_CryptoChainBatch(benchmark::State& state, const char* method) {
    if (!Ciphertext::Support(ppp::string(method))) {
        state.SkipWithError("cipher method not supported");
        return;
    }

    Chain chain(method);
    const int count = static_cast<int>(state.range(0));
    std::vector<Byte> payload(1400, 0x2a);
    std::vector<std::shared_ptr<Byte>> outputs;

    if (!api_batch_matches_single(chain, 1400, count) ||
        !chain.encrypt_many(payload.data(), static_cast<int>(payload.size()), count, outputs)) {
        state.SkipWithError("batched crypto-chain self-check failed");
        return;
    }

    for (auto _ : state) {
        if (!chain.encrypt_many(payload.data(), static_cast<int>(payload.size()), count, outputs)) {
            state.SkipWithError("batched crypto-chain execution failed");
            return;
        }
        benchmark::DoNotOptimize(outputs.data());
        benchmark::DoNotOptimize(outputs[0].get());
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * static_cast<int64_t>(payload.size()));
    state.counters["batch"] = count;
    state.counters["allocations"] = count;
}
} // namespace

#define REGISTER(name, method) \
    BENCHMARK_CAPTURE(BM_CryptoChain, name, method)->Arg(64)->Arg(1400)
REGISTER(openssl, "aes-256-cfb");
REGISTER(simd, "simd-aes-256-cfb");
#undef REGISTER
BENCHMARK_CAPTURE(BM_CryptoChainBatch, simd, "simd-aes-256-cfb")
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->UseRealTime();
BENCHMARK_MAIN();
