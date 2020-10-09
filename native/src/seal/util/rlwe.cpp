// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include "seal/ciphertext.h"
#include "seal/randomgen.h"
#include "seal/randomtostd.h"
#include "seal/util/clipnormal.h"
#include "seal/util/common.h"
#include "seal/util/globals.h"
#include "seal/util/ntt.h"
#include "seal/util/polyarithsmallmod.h"
#include "seal/util/polycore.h"
#include "seal/util/rlwe.h"

using namespace std;

namespace seal
{
    namespace util
    {
        void sample_poly_ternary(
            shared_ptr<UniformRandomGenerator> rng, const EncryptionParameters &parms, uint64_t *destination)
        {
            auto coeff_modulus = parms.coeff_modulus();
            size_t coeff_modulus_size = coeff_modulus.size();
            size_t coeff_count = parms.poly_modulus_degree();
            RandomToStandardAdapter engine(rng);
            uniform_int_distribution<uint64_t> dist(0, 2);

            SEAL_ITERATE(iter(destination), coeff_count, [&](auto &I) {
                uint64_t rand = dist(engine);
                uint64_t flag = static_cast<uint64_t>(-static_cast<int64_t>(rand == 0));
                SEAL_ITERATE(
                    iter(StrideIter<uint64_t *>(&I, coeff_count), coeff_modulus), coeff_modulus_size,
                    [&](auto J) { *get<0>(J) = rand + (flag & get<1>(J).value()) - 1; });
            });
        }

        void sample_poly_normal(
            shared_ptr<UniformRandomGenerator> rng, const EncryptionParameters &parms, uint64_t *destination)
        {
            auto coeff_modulus = parms.coeff_modulus();
            size_t coeff_modulus_size = coeff_modulus.size();
            size_t coeff_count = parms.poly_modulus_degree();

            if (are_close(global_variables::noise_max_deviation, 0.0))
            {
                set_zero_poly(coeff_count, coeff_modulus_size, destination);
                return;
            }

            RandomToStandardAdapter engine(rng);
            ClippedNormalDistribution dist(
                0, global_variables::noise_standard_deviation, global_variables::noise_max_deviation);

            SEAL_ITERATE(iter(destination), coeff_count, [&](auto &I) {
                int64_t noise = static_cast<int64_t>(dist(engine));
                uint64_t flag = static_cast<uint64_t>(-static_cast<int64_t>(noise < 0));
                SEAL_ITERATE(
                    iter(StrideIter<uint64_t *>(&I, coeff_count), coeff_modulus), coeff_modulus_size,
                    [&](auto J) { *get<0>(J) = static_cast<uint64_t>(noise) + (flag & get<1>(J).value()); });
            });
        }

        void sample_poly_cbd(
            shared_ptr<UniformRandomGenerator> rng, const EncryptionParameters &parms, uint64_t *destination)
        {
            auto coeff_modulus = parms.coeff_modulus();
            size_t coeff_modulus_size = coeff_modulus.size();
            size_t coeff_count = parms.poly_modulus_degree();

            if (are_close(global_variables::noise_max_deviation, 0.0))
            {
                set_zero_poly(coeff_count, coeff_modulus_size, destination);
                return;
            }

            if (global_variables::noise_standard_deviation != 3.2)
            {
                throw logic_error(
                    "center binomial distribution only supports standard deviation 3.2, use discrete Gaussian instead");
            }

            auto hw = [](int8_t x) {
                int32_t t = x & 0x55;
                t += (x >> 1) & 0x55; // counting every 2 bits
                int32_t hw = t & 0x3;
                hw += (t >> 2) & 0x3;
                hw += (t >> 4) & 0x3;
                hw += (t >> 6) & 0x3;
                return hw;
            };

            auto cbd = [&](shared_ptr<UniformRandomGenerator> rng) {
                int8_t x[6];
                rng->generate(6, reinterpret_cast<seal_byte *>(x));
                x[2] &= 0x1F;
                x[5] &= 0x1F;
                return hw(x[0]) + hw(x[1]) + hw(x[2]) - hw(x[3]) - hw(x[4]) - hw(x[5]);
            };

            SEAL_ITERATE(iter(destination), coeff_count, [&](auto &I) {
                int32_t noise = cbd(rng);
                uint64_t flag = static_cast<uint64_t>(-static_cast<int64_t>(noise < 0));
                SEAL_ITERATE(
                    iter(StrideIter<uint64_t *>(&I, coeff_count), coeff_modulus), coeff_modulus_size,
                    [&](auto J) { *get<0>(J) = static_cast<uint64_t>(noise) + (flag & get<1>(J).value()); });
            });
        }

        void sample_poly_uniform(
            shared_ptr<UniformRandomGenerator> rng, const EncryptionParameters &parms, uint64_t *destination)
        {
            // Extract encryption parameters.
            auto coeff_modulus = parms.coeff_modulus();
            size_t coeff_modulus_size = coeff_modulus.size();
            size_t coeff_count = parms.poly_modulus_degree();

            // Set up source of randomness that produces 32 bit random things.
            RandomToStandardAdapter engine(rng);

            constexpr uint64_t max_random = static_cast<uint64_t>(0xFFFFFFFFFFFFFFFFULL);
            for (size_t j = 0; j < coeff_modulus_size; j++)
            {
                auto &modulus = coeff_modulus[j];
                uint64_t max_multiple = max_random - barrett_reduce_64(max_random, modulus) - 1;
                for (size_t i = 0; i < coeff_count; i++)
                {
                    // This ensures uniform distribution.
                    uint64_t rand;
                    do
                    {
                        rand = (static_cast<uint64_t>(engine()) << 32) | static_cast<uint64_t>(engine());
                    } while (rand >= max_multiple);
                    destination[i + j * coeff_count] = barrett_reduce_64(rand, modulus);
                }
            }
        }

        void encrypt_zero_asymmetric(
            const PublicKey &public_key, const SEALContext &context, parms_id_type parms_id, bool is_ntt_form,
            Ciphertext &destination)
        {
#ifdef SEAL_DEBUG
            if (!is_valid_for(public_key, context))
            {
                throw invalid_argument("public key is not valid for the encryption parameters");
            }
#endif
            // We use a fresh memory pool with `clear_on_destruction' enabled.
            MemoryPoolHandle pool = MemoryManager::GetPool(mm_prof_opt::FORCE_NEW, true);

            auto &context_data = *context.get_context_data(parms_id);
            auto &parms = context_data.parms();
            auto &coeff_modulus = parms.coeff_modulus();
            size_t coeff_modulus_size = coeff_modulus.size();
            size_t coeff_count = parms.poly_modulus_degree();
            auto ntt_tables = context_data.small_ntt_tables();
            size_t encrypted_size = public_key.data().size();

            // Make destination have right size and parms_id
            // Ciphertext (c_0,c_1, ...)
            destination.resize(context, parms_id, encrypted_size);
            destination.is_ntt_form() = is_ntt_form;
            destination.scale() = 1.0;

            // c[j] = public_key[j] * u + e[j] where e[j] <-- chi, u <-- R_3.

            // Create RNG, u and error share one RNG.
            auto rng = parms.random_generator()->create();

            // Generate u <-- R_3
            auto u(allocate_poly(coeff_count, coeff_modulus_size, pool));
            sample_poly_ternary(rng, parms, u.get());

            // c[j] = u * public_key[j]
            for (size_t i = 0; i < coeff_modulus_size; i++)
            {
                ntt_negacyclic_harvey(u.get() + i * coeff_count, ntt_tables[i]);
                for (size_t j = 0; j < encrypted_size; j++)
                {
                    dyadic_product_coeffmod(
                        u.get() + i * coeff_count, public_key.data().data(j) + i * coeff_count, coeff_count,
                        coeff_modulus[i], destination.data(j) + i * coeff_count);

                    // Addition with e_0, e_1 is in non-NTT form.
                    if (!is_ntt_form)
                    {
                        inverse_ntt_negacyclic_harvey(destination.data(j) + i * coeff_count, ntt_tables[i]);
                    }
                }
            }

            // Generate e_j <-- chi.
            // c[j] = public_key[j] * u + e[j]
            for (size_t j = 0; j < encrypted_size; j++)
            {
#ifdef SEAL_USE_GAUSSIAN
                sample_poly_normal(rng, parms, u.get());
#else
                sample_poly_cbd(rng, parms, u.get());
#endif
                for (size_t i = 0; i < coeff_modulus_size; i++)
                {
                    // Addition with e_0, e_1 is in NTT form.
                    if (is_ntt_form)
                    {
                        ntt_negacyclic_harvey(u.get() + i * coeff_count, ntt_tables[i]);
                    }
                    add_poly_coeffmod(
                        u.get() + i * coeff_count, destination.data(j) + i * coeff_count, coeff_count, coeff_modulus[i],
                        destination.data(j) + i * coeff_count);
                }
            }
        }

        void encrypt_zero_symmetric(
            const SecretKey &secret_key, const SEALContext &context, parms_id_type parms_id, bool is_ntt_form,
            bool save_seed, Ciphertext &destination)
        {
#ifdef SEAL_DEBUG
            if (!is_valid_for(secret_key, context))
            {
                throw invalid_argument("secret key is not valid for the encryption parameters");
            }
#endif
            // We use a fresh memory pool with `clear_on_destruction' enabled.
            MemoryPoolHandle pool = MemoryManager::GetPool(mm_prof_opt::FORCE_NEW, true);

            auto &context_data = *context.get_context_data(parms_id);
            auto &parms = context_data.parms();
            auto &coeff_modulus = parms.coeff_modulus();
            size_t coeff_modulus_size = coeff_modulus.size();
            size_t coeff_count = parms.poly_modulus_degree();
            auto ntt_tables = context_data.small_ntt_tables();
            size_t encrypted_size = 2;

            // If a polynomial is too small to store a seed, disable save_seed.
            auto poly_uint64_count = mul_safe(coeff_count, coeff_modulus_size);
            if (save_seed && static_cast<uint64_t>(poly_uint64_count) < (random_seed_type().size() + 1))
            {
                save_seed = false;
            }

            destination.resize(context, parms_id, encrypted_size);
            destination.is_ntt_form() = is_ntt_form;
            destination.scale() = 1.0;

            // Create an instance of a random number generator. We use this for sampling a seed for a second BlakePRNG
            // used for sampling u (the seed can be public information. This RNG is also used for sampling the error.
            auto bootstrap_rng = parms.random_generator()->create();

            // Sample a seed for generating uniform randomness for the ciphertext; this seed is public information
            random_seed_type public_rng_seed;
            bootstrap_rng->generate(sizeof(random_seed_type), reinterpret_cast<seal_byte *>(public_rng_seed.data()));

            // Create a BlakePRNG for sampling u
            auto ciphertext_rng = BlakePRNGFactory(public_rng_seed).create();

            // Generate ciphertext: (c[0], c[1]) = ([-(as+e)]_q, a)
            uint64_t *c0 = destination.data();
            uint64_t *c1 = destination.data(1);

            // Sample a uniformly at random
            if (is_ntt_form || !save_seed)
            {
                // Sample the NTT form directly
                sample_poly_uniform(ciphertext_rng, parms, c1);
            }
            else if (save_seed)
            {
                // Sample non-NTT form and store the seed
                sample_poly_uniform(ciphertext_rng, parms, c1);
                for (size_t i = 0; i < coeff_modulus_size; i++)
                {
                    // Transform the c1 into NTT representation.
                    ntt_negacyclic_harvey(c1 + i * coeff_count, ntt_tables[i]);
                }
            }

            // Sample e <-- chi
            auto noise(allocate_poly(coeff_count, coeff_modulus_size, pool));
#ifdef SEAL_USE_GAUSSIAN
            sample_poly_normal(bootstrap_rng, parms, noise.get());
#else
            sample_poly_cbd(bootstrap_rng, parms, noise.get());
#endif
            // Calculate -(a*s + e) (mod q) and store in c[0]
            for (size_t i = 0; i < coeff_modulus_size; i++)
            {
                dyadic_product_coeffmod(
                    secret_key.data().data() + i * coeff_count, c1 + i * coeff_count, coeff_count, coeff_modulus[i],
                    c0 + i * coeff_count);
                if (is_ntt_form)
                {
                    // Transform the noise e into NTT representation.
                    ntt_negacyclic_harvey(noise.get() + i * coeff_count, ntt_tables[i]);
                }
                else
                {
                    inverse_ntt_negacyclic_harvey(c0 + i * coeff_count, ntt_tables[i]);
                }
                add_poly_coeffmod(
                    noise.get() + i * coeff_count, c0 + i * coeff_count, coeff_count, coeff_modulus[i],
                    c0 + i * coeff_count);
                negate_poly_coeffmod(c0 + i * coeff_count, coeff_count, coeff_modulus[i], c0 + i * coeff_count);
            }

            if (!is_ntt_form && !save_seed)
            {
                for (size_t i = 0; i < coeff_modulus_size; i++)
                {
                    // Transform the c1 into non-NTT representation.
                    inverse_ntt_negacyclic_harvey(c1 + i * coeff_count, ntt_tables[i]);
                }
            }

            if (save_seed)
            {
                // Write random seed to destination.data(1).
                c1[0] = static_cast<uint64_t>(0xFFFFFFFFFFFFFFFFULL);
                copy_n(public_rng_seed.cbegin(), public_rng_seed.size(), c1 + 1);
            }
        }
    } // namespace util
} // namespace seal
