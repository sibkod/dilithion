// Copyright (c) 2025 The Dilithion Core developers
// Distributed under the MIT software license

#ifndef DILITHION_RPC_AUTH_H
#define DILITHION_RPC_AUTH_H

#include <string>
#include <vector>
#include <cstdint>

/**
 * RPC Authentication Module
 *
 * Implements HTTP Basic Authentication for RPC server security.
 * Uses SHA-3-256 for password hashing (quantum-resistant).
 *
 * Security Model:
 * - Username and password stored in dilithion.conf
 * - Password hashed with SHA-3-256 + salt
 * - HTTP Basic Auth for transport
 * - All unauthenticated requests rejected with HTTP 401
 *
 * Future Enhancements:
 * - Rate limiting (prevent brute force)
 * - TLS/HTTPS support
 * - API key authentication
 * - Request signing
 */

namespace RPCAuth {

/**
 * @brief Generate cryptographically secure random salt
 *
 * Generates 32 bytes of random data for password salting.
 *
 * @param salt Output buffer for salt (32 bytes)
 * @return true if successful, false on error
 *
 * @note Uses /dev/urandom on Unix or CryptGenRandom on Windows
 */
bool GenerateSalt(std::vector<uint8_t>& salt);

/**
 * @brief Hash password with PBKDF2-HMAC-SHA3-256
 *
 * Creates a secure password hash using PBKDF2 key derivation function.
 * Uses: hash = PBKDF2-HMAC-SHA3-256(password, salt, 100000 iterations)
 *
 * @param password Plain text password
 * @param salt Random salt (32 bytes recommended)
 * @param hashOut Output hash (32 bytes)
 * @return true if successful, false on error
 *
 * @note RPC-005 FIX: Uses PBKDF2 with 100,000 iterations (OWASP recommendation)
 *       Resistant to GPU brute-force attacks. Each hash takes ~100ms.
 */
bool HashPassword(const std::string& password,
                  const std::vector<uint8_t>& salt,
                  std::vector<uint8_t>& hashOut);

/**
 * @brief PBKDF2-HMAC-SHA3-256 key derivation function
 *
 * Implements PBKDF2 (Password-Based Key Derivation Function 2) using HMAC-SHA3-256
 * as the pseudorandom function. Resistant to brute-force attacks.
 *
 * @param password Plain text password
 * @param passwordLen Length of password in bytes
 * @param salt Random salt
 * @param saltLen Length of salt in bytes
 * @param iterations Number of PBKDF2 iterations (100,000+ recommended)
 * @param dkOut Output derived key buffer (must be allocated)
 * @param dkLen Length of derived key to generate
 * @return true if successful, false on error
 *
 * @note RPC-005 FIX: Replaces weak single-round SHA3-256 hashing
 */
bool PBKDF2_HMAC_SHA3(const uint8_t* password, size_t passwordLen,
                       const uint8_t* salt, size_t saltLen,
                       uint32_t iterations,
                       uint8_t* dkOut, size_t dkLen);

/**
 * @brief Verify password against stored hash
 *
 * Computes hash of provided password and compares with stored hash.
 * Uses constant-time comparison to prevent timing attacks.
 *
 * @param password Plain text password to verify
 * @param salt Salt used in original hash
 * @param storedHash Hash to compare against
 * @return true if password matches, false otherwise
 *
 * @note Uses constant-time comparison
 */
bool VerifyPassword(const std::string& password,
                    const std::vector<uint8_t>& salt,
                    const std::vector<uint8_t>& storedHash);

/**
 * @brief Parse HTTP Basic Auth header
 *
 * Parses "Authorization: Basic base64(username:password)" header.
 *
 * Format: "Basic dXNlcm5hbWU6cGFzc3dvcmQ="
 *         where base64 decodes to "username:password"
 *
 * @param authHeader Full authorization header value
 * @param username Output username
 * @param password Output password
 * @return true if parsed successfully, false if malformed
 *
 * @note Does not validate credentials, only parses header
 */
bool ParseAuthHeader(const std::string& authHeader,
                     std::string& username,
                     std::string& password);

/**
 * @brief Check if credentials are valid
 *
 * Validates username and password against configured values.
 * Reads configuration from global settings.
 *
 * @param username Username to check
 * @param password Password to check
 * @return true if credentials valid, false otherwise
 *
 * @note Thread-safe
 */
bool AuthenticateRequest(const std::string& username,
                         const std::string& password);

/**
 * @brief Initialize authentication system
 *
 * Loads rpcuser and rpcpassword from configuration.
 * Must be called before authentication can work.
 *
 * @param configUser Username from config
 * @param configPassword Password from config
 * @return true if initialized successfully
 *
 * @note Call once at server startup
 */
bool InitializeAuth(const std::string& configUser,
                    const std::string& configPassword);

/**
 * @brief Check if authentication is configured
 *
 * @return true if rpcuser and rpcpassword are set
 */
bool IsAuthConfigured();

/**
 * @brief Base64 encode data
 *
 * Encodes binary data to Base64 string.
 *
 * @param data Input data
 * @param dataLen Length of input data
 * @return Base64 encoded string
 */
std::string Base64Encode(const uint8_t* data, size_t dataLen);

/**
 * @brief Base64 decode string
 *
 * Decodes Base64 string to binary data.
 *
 * @param encoded Base64 encoded string
 * @param decoded Output vector for decoded data
 * @return true if decoded successfully, false if invalid Base64
 */
bool Base64Decode(const std::string& encoded, std::vector<uint8_t>& decoded);

/**
 * @brief Constant-time memory comparison
 *
 * Compares two memory regions in constant time to prevent timing attacks.
 *
 * @param a First buffer
 * @param b Second buffer
 * @param len Length to compare
 * @return true if equal, false if different
 *
 * @note Always compares full length regardless of differences
 */
bool SecureCompare(const uint8_t* a, const uint8_t* b, size_t len);

/**
 * @brief Result of a cached auth lookup.
 */
enum class CachedAuthResult {
    HitSuccess,   ///< cache hit, credentials previously verified OK
    HitFail,      ///< cache hit, credentials previously verified BAD
    Miss          ///< no cache entry — caller must run full PBKDF2 path
};

/**
 * @brief Try to satisfy an auth request from the in-memory credential cache.
 *
 * Cache key is SHA3-256(username || 0x00 || password). The cache is bounded
 * (256 entries, FIFO eviction), thread-safe, and TTL-bounded
 * (success: 60s, failure: 5s). The cache prevents repeated PBKDF2-HMAC-SHA3
 * (100k iters, ~50–100 ms CPU per call) for high-rate same-credential
 * traffic — without the cache, sustained Explorer load on the same RPC
 * credential saturates a 2-vCPU host and produces a CLOSE-WAIT socket leak
 * because clients FIN before the server finishes PBKDF2 on a dead request.
 *
 * On HitSuccess: permissionsOut is populated with the cached permissions
 * bitmask (as stored by CacheAuthSuccess).
 *
 * @param username Plaintext username from Basic auth.
 * @param password Plaintext password from Basic auth.
 * @param permissionsOut Output: permissions bitmask if HitSuccess.
 * @return CachedAuthResult — HitSuccess, HitFail, or Miss.
 *
 * @note On Miss, callers must run the full RPCAuth::AuthenticateRequest +
 *       CRPCPermissions::AuthenticateUser path and call CacheAuthSuccess /
 *       CacheAuthFail to populate the cache.
 * @note Lockout / rate-limit checks are NOT bypassed: callers must check
 *       lockout BEFORE consulting the cache.
 */
CachedAuthResult TryCachedAuth(const std::string& username,
                               const std::string& password,
                               uint32_t& permissionsOut);

/**
 * @brief Insert a verified-good credential into the auth cache.
 *
 * TTL: 60 seconds. Inserts evict the oldest entry if the cache is full.
 *
 * @param username Plaintext username.
 * @param password Plaintext password.
 * @param permissions Permissions bitmask resolved via CRPCPermissions.
 */
void CacheAuthSuccess(const std::string& username,
                      const std::string& password,
                      uint32_t permissions);

/**
 * @brief Insert a verified-bad credential into the auth cache.
 *
 * TTL: 5 seconds (shorter window: limits negative-cache abuse vector
 * for legitimate user typos).
 *
 * @param username Plaintext username.
 * @param password Plaintext password.
 */
void CacheAuthFail(const std::string& username,
                   const std::string& password);

/**
 * @brief Clear all cached credentials. Used by tests and on credential rotation.
 */
void ClearAuthCache();

} // namespace RPCAuth

#endif // DILITHION_RPC_AUTH_H
