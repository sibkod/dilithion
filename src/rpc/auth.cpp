// Copyright (c) 2025 The Dilithion Core developers
// Distributed under the MIT software license

#include <rpc/auth.h>
#include <crypto/sha3.h>

#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
    #include <wincrypt.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace RPCAuth {

// Forward declarations
static void HMAC_SHA3_256(const uint8_t* key, size_t keyLen,
                          const uint8_t* data, size_t dataLen,
                          uint8_t* macOut);

// Global authentication configuration
static std::string g_rpcUser;
static std::string g_rpcPassword;
static std::vector<uint8_t> g_passwordSalt;
static std::vector<uint8_t> g_passwordHash;
static bool g_authConfigured = false;
static std::mutex g_authMutex;

// =============================================================================
// Auth credential cache
// =============================================================================
// PBKDF2-HMAC-SHA3-256 with 100,000 iterations costs ~50–100 ms CPU per call,
// and was being invoked TWICE per request (RPCAuth::AuthenticateRequest plus
// CRPCPermissions::AuthenticateUser). On a 2-vCPU host sustaining Explorer +
// bridge-relayer + external RPC traffic against localhost, this saturated the
// worker pool, the request queue grew, clients (curl with 3–10 s timeouts) sent
// FIN before the server responded, and the resulting CLOSE-WAITs accumulated
// faster than they could be closed. Cache hits skip both PBKDF2 calls.
//
// Cache key: SHA3-256(user || 0x00 || password). Plaintext is never stored.
// TTL: success 60 s, failure 5 s. Bounded at 256 entries, FIFO eviction.
//
// Lockout / rate-limit checks are NOT bypassed: TryCachedAuth callers MUST
// check m_rateLimiter.IsLockedOut BEFORE consulting the cache.

namespace {

constexpr size_t  AUTH_CACHE_MAX_ENTRIES   = 256;
constexpr int64_t AUTH_CACHE_TTL_OK_MS     = 60 * 1000;
constexpr int64_t AUTH_CACHE_TTL_FAIL_MS   = 5 * 1000;

struct AuthCacheEntry {
    std::array<uint8_t, 32> key;
    std::chrono::steady_clock::time_point expiry;
    uint32_t permissions;
    bool success;
};

static std::mutex g_authCacheMutex;
static std::deque<AuthCacheEntry> g_authCache;  // FIFO

// Derive the cache key from (username, password). The output is a 32-byte
// SHA3-256 digest. The plaintext credentials are zeroed from the local
// intermediate buffer before return.
void DeriveCacheKey(const std::string& username,
                    const std::string& password,
                    std::array<uint8_t, 32>& keyOut) {
    std::vector<uint8_t> buf;
    buf.reserve(username.size() + 1 + password.size());
    buf.insert(buf.end(), username.begin(), username.end());
    buf.push_back(0x00);
    buf.insert(buf.end(), password.begin(), password.end());
    SHA3_256(buf.data(), buf.size(), keyOut.data());
    // Zero the intermediate buffer (best-effort; std::vector growth elsewhere
    // may have left earlier copies behind, but new data only flowed through
    // this single buffer).
    if (!buf.empty()) {
        std::memset(buf.data(), 0, buf.size());
    }
}

// Internal helper: scan the cache with constant-time key comparison.
// Returns iterator to a matching, non-expired entry, or g_authCache.end().
// Caller MUST hold g_authCacheMutex.
std::deque<AuthCacheEntry>::iterator FindLive(
    const std::array<uint8_t, 32>& key,
    std::chrono::steady_clock::time_point now)
{
    for (auto it = g_authCache.begin(); it != g_authCache.end(); ++it) {
        if (it->expiry <= now) continue;
        if (RPCAuth::SecureCompare(it->key.data(), key.data(), 32)) {
            return it;
        }
    }
    return g_authCache.end();
}

// Evict any entries whose expiry has passed. Caller MUST hold mutex.
void EvictExpired(std::chrono::steady_clock::time_point now) {
    while (!g_authCache.empty() && g_authCache.front().expiry <= now) {
        // Wipe key before pop (defense in depth — no plaintext in here, but
        // a digest of plaintext is still sensitive).
        std::memset(g_authCache.front().key.data(), 0, 32);
        g_authCache.pop_front();
    }
}

// Insert a new entry. If the cache is full, evict the oldest entry. Caller
// MUST hold mutex.
void InsertEntry(const AuthCacheEntry& entry) {
    if (g_authCache.size() >= AUTH_CACHE_MAX_ENTRIES) {
        std::memset(g_authCache.front().key.data(), 0, 32);
        g_authCache.pop_front();
    }
    g_authCache.push_back(entry);
}

} // namespace

// Base64 encoding table
static const char* BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

bool GenerateSalt(std::vector<uint8_t>& salt) {
    salt.resize(32);

#ifdef _WIN32
    // RPC-015 FIX: Use BCryptGenRandom instead of deprecated CryptGenRandom
    // BCryptGenRandom is the modern Windows cryptographic RNG (Windows Vista+)
    // Falls back to CryptGenRandom only if BCryptGenRandom unavailable
    #if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0600
        // Windows Vista+ : Use BCryptGenRandom (preferred)
        #include <bcrypt.h>
        #pragma comment(lib, "bcrypt.lib")

        NTSTATUS status = BCryptGenRandom(
            NULL,                   // Use default RNG algorithm
            salt.data(),            // Output buffer
            32,                     // Number of bytes
            BCRYPT_USE_SYSTEM_PREFERRED_RNG  // Use system-preferred RNG
        );
        return status == 0;  // STATUS_SUCCESS = 0
    #else
        // Windows XP fallback: Use CryptGenRandom (deprecated but necessary for old systems)
        HCRYPTPROV hProvider = 0;
        if (!CryptAcquireContextW(&hProvider, nullptr, nullptr,
                                  PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            return false;
        }

        BOOL result = CryptGenRandom(hProvider, 32, salt.data());
        CryptReleaseContext(hProvider, 0);

        return result != 0;
    #endif
#else
    // Unix: Use /dev/urandom
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }

    ssize_t bytesRead = read(fd, salt.data(), 32);
    close(fd);

    return bytesRead == 32;
#endif
}

// RPC-005 FIX: PBKDF2-HMAC-SHA3-256 implementation
// Replaces weak single-round SHA3-256 with proper key derivation function
bool PBKDF2_HMAC_SHA3(const uint8_t* password, size_t passwordLen,
                       const uint8_t* salt, size_t saltLen,
                       uint32_t iterations,
                       uint8_t* dkOut, size_t dkLen) {
    // PBKDF2 algorithm (RFC 2898):
    // DK = PBKDF2(PRF, Password, Salt, iterations, dkLen)
    // where PRF = HMAC-SHA3-256

    if (!password || !salt || !dkOut || passwordLen == 0 || saltLen == 0 ||
        iterations == 0 || dkLen == 0) {
        return false;
    }

    const size_t hLen = 32;  // SHA3-256 output size
    uint32_t numBlocks = (dkLen + hLen - 1) / hLen;

    std::vector<uint8_t> derivedKey;
    derivedKey.reserve(numBlocks * hLen);

    // For each block
    for (uint32_t blockIndex = 1; blockIndex <= numBlocks; blockIndex++) {
        // U_1 = HMAC(password, salt || INT_32_BE(blockIndex))
        std::vector<uint8_t> saltBlock(salt, salt + saltLen);
        saltBlock.push_back((blockIndex >> 24) & 0xFF);
        saltBlock.push_back((blockIndex >> 16) & 0xFF);
        saltBlock.push_back((blockIndex >> 8) & 0xFF);
        saltBlock.push_back(blockIndex & 0xFF);

        // Compute initial U_1 = HMAC-SHA3-256(password, salt || blockIndex)
        std::vector<uint8_t> U(hLen);
        HMAC_SHA3_256(password, passwordLen, saltBlock.data(), saltBlock.size(), U.data());

        // T = U_1
        std::vector<uint8_t> T = U;

        // For iterations 2..c: U_i = HMAC(password, U_{i-1}), T = T XOR U_i
        for (uint32_t iter = 1; iter < iterations; iter++) {
            HMAC_SHA3_256(password, passwordLen, U.data(), U.size(), U.data());
            for (size_t j = 0; j < hLen; j++) {
                T[j] ^= U[j];
            }
        }

        // Append T to derived key
        derivedKey.insert(derivedKey.end(), T.begin(), T.end());

        // RPC-022 FIX: Secure memory cleanup
        memset(U.data(), 0, U.size());
        memset(T.data(), 0, T.size());
        memset(saltBlock.data(), 0, saltBlock.size());
    }

    // Copy requested length to output
    memcpy(dkOut, derivedKey.data(), dkLen);

    // RPC-022 FIX: Secure erase derived key from memory
    memset(derivedKey.data(), 0, derivedKey.size());

    return true;
}

// Helper: HMAC-SHA3-256 (used by PBKDF2)
static void HMAC_SHA3_256(const uint8_t* key, size_t keyLen,
                          const uint8_t* data, size_t dataLen,
                          uint8_t* macOut) {
    const size_t blockSize = 136;  // SHA3-256 block size (1088 bits / 8)
    const size_t hashSize = 32;     // SHA3-256 output size

    // Prepare key
    std::vector<uint8_t> keyPadded(blockSize, 0);
    if (keyLen <= blockSize) {
        memcpy(keyPadded.data(), key, keyLen);
    } else {
        // If key > blockSize, hash it first
        SHA3_256(key, keyLen, keyPadded.data());
    }

    // Compute o_key_pad = key XOR 0x5c
    std::vector<uint8_t> oKeyPad(blockSize);
    for (size_t i = 0; i < blockSize; i++) {
        oKeyPad[i] = keyPadded[i] ^ 0x5c;
    }

    // Compute i_key_pad = key XOR 0x36
    std::vector<uint8_t> iKeyPad(blockSize);
    for (size_t i = 0; i < blockSize; i++) {
        iKeyPad[i] = keyPadded[i] ^ 0x36;
    }

    // Inner hash: H(i_key_pad || data)
    std::vector<uint8_t> innerInput;
    innerInput.reserve(blockSize + dataLen);
    innerInput.insert(innerInput.end(), iKeyPad.begin(), iKeyPad.end());
    innerInput.insert(innerInput.end(), data, data + dataLen);

    uint8_t innerHash[hashSize];
    SHA3_256(innerInput.data(), innerInput.size(), innerHash);

    // Outer hash: H(o_key_pad || innerHash)
    std::vector<uint8_t> outerInput;
    outerInput.reserve(blockSize + hashSize);
    outerInput.insert(outerInput.end(), oKeyPad.begin(), oKeyPad.end());
    outerInput.insert(outerInput.end(), innerHash, innerHash + hashSize);

    SHA3_256(outerInput.data(), outerInput.size(), macOut);

    // RPC-022 FIX: Secure memory cleanup
    memset(keyPadded.data(), 0, keyPadded.size());
    memset(oKeyPad.data(), 0, oKeyPad.size());
    memset(iKeyPad.data(), 0, iKeyPad.size());
    memset(innerInput.data(), 0, innerInput.size());
    memset(innerHash, 0, hashSize);
    memset(outerInput.data(), 0, outerInput.size());
}

bool HashPassword(const std::string& password,
                  const std::vector<uint8_t>& salt,
                  std::vector<uint8_t>& hashOut) {
    // RPC-005 FIX: Use PBKDF2-HMAC-SHA3-256 with 100,000 iterations
    // OWASP recommendation: 100,000 iterations for PBKDF2-HMAC-SHA256
    // Provides strong resistance to brute-force and GPU attacks

    // Input validation
    if (password.empty() || salt.empty()) {
        return false;
    }

    const uint32_t PBKDF2_ITERATIONS = 100000;  // OWASP recommendation (2023)

    // Derive 32-byte key using PBKDF2
    hashOut.resize(32);
    bool result = PBKDF2_HMAC_SHA3(
        reinterpret_cast<const uint8_t*>(password.c_str()),
        password.length(),
        salt.data(),
        salt.size(),
        PBKDF2_ITERATIONS,
        hashOut.data(),
        32
    );

    // RPC-022 FIX: Note - password parameter will be cleared by caller
    // No local sensitive data to clean (password is const reference)

    return result;
}

bool VerifyPassword(const std::string& password,
                    const std::vector<uint8_t>& salt,
                    const std::vector<uint8_t>& storedHash) {
    // Input validation
    if (password.empty() || salt.empty() || storedHash.size() != 32) {
        return false;
    }

    // Compute hash of provided password
    std::vector<uint8_t> computedHash;
    if (!HashPassword(password, salt, computedHash)) {
        return false;
    }

    // Constant-time comparison
    bool result = SecureCompare(computedHash.data(), storedHash.data(), 32);

    // Secure erase computed hash
    if (!computedHash.empty()) {
        memset(computedHash.data(), 0, computedHash.size());
    }

    return result;
}

std::string Base64Encode(const uint8_t* data, size_t dataLen) {
    std::string result;
    result.reserve(((dataLen + 2) / 3) * 4);

    for (size_t i = 0; i < dataLen; i += 3) {
        uint32_t triple = (data[i] << 16);
        if (i + 1 < dataLen) triple |= (data[i + 1] << 8);
        if (i + 2 < dataLen) triple |= data[i + 2];

        result.push_back(BASE64_CHARS[(triple >> 18) & 0x3F]);
        result.push_back(BASE64_CHARS[(triple >> 12) & 0x3F]);
        result.push_back(i + 1 < dataLen ? BASE64_CHARS[(triple >> 6) & 0x3F] : '=');
        result.push_back(i + 2 < dataLen ? BASE64_CHARS[triple & 0x3F] : '=');
    }

    // MAINNET FIX: Return without std::move to allow RVO
    return result;
}

bool Base64Decode(const std::string& encoded, std::vector<uint8_t>& decoded) {
    // Build decode table
    static int decodeTable[256];
    static bool tableInitialized = false;

    if (!tableInitialized) {
        std::fill(std::begin(decodeTable), std::end(decodeTable), -1);
        for (int i = 0; i < 64; i++) {
            decodeTable[static_cast<uint8_t>(BASE64_CHARS[i])] = i;
        }
        tableInitialized = true;
    }

    decoded.clear();
    decoded.reserve(encoded.length() * 3 / 4);

    uint32_t value = 0;
    int bits = 0;

    for (char c : encoded) {
        if (c == '=') break;  // Padding
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;  // Skip whitespace

        int v = decodeTable[static_cast<uint8_t>(c)];
        if (v < 0) return false;  // Invalid character

        value = (value << 6) | v;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            decoded.push_back((value >> bits) & 0xFF);
        }
    }

    return true;
}

bool ParseAuthHeader(const std::string& authHeader,
                     std::string& username,
                     std::string& password) {
    // Expected format: "Basic <base64(username:password)>"

    // Check for "Basic " prefix (case-insensitive)
    std::string prefix = "Basic ";
    if (authHeader.length() < prefix.length()) {
        return false;
    }

    if (authHeader.substr(0, prefix.length()) != prefix) {
        return false;
    }

    // Extract base64 part
    std::string base64Part = authHeader.substr(prefix.length());

    // Decode base64
    std::vector<uint8_t> decoded;
    if (!Base64Decode(base64Part, decoded)) {
        return false;
    }

    // Convert to string
    std::string credentials(decoded.begin(), decoded.end());

    // Find ':' separator
    size_t colonPos = credentials.find(':');
    if (colonPos == std::string::npos) {
        return false;  // No ':' found
    }

    // Extract username and password
    username = credentials.substr(0, colonPos);
    password = credentials.substr(colonPos + 1);

    return true;
}

bool InitializeAuth(const std::string& configUser,
                    const std::string& configPassword) {
    std::lock_guard<std::mutex> lock(g_authMutex);

    // Validate inputs
    if (configUser.empty() || configPassword.empty()) {
        g_authConfigured = false;
        return false;
    }

    // Store username
    g_rpcUser = configUser;
    g_rpcPassword = configPassword;

    // Generate salt for password hashing
    if (!GenerateSalt(g_passwordSalt)) {
        g_authConfigured = false;
        return false;
    }

    // Hash the password
    if (!HashPassword(configPassword, g_passwordSalt, g_passwordHash)) {
        g_authConfigured = false;
        return false;
    }

    // v4.4.2 leak fix: drop any stale cached entries from a prior init
    // so a credential rotation (re-init with new password) takes effect
    // immediately rather than after the 60 s success-TTL elapses.
    ClearAuthCache();

    g_authConfigured = true;
    return true;
}

bool IsAuthConfigured() {
    std::lock_guard<std::mutex> lock(g_authMutex);
    return g_authConfigured;
}

bool AuthenticateRequest(const std::string& username,
                         const std::string& password) {
    std::lock_guard<std::mutex> lock(g_authMutex);

    // Check if authentication is configured
    if (!g_authConfigured) {
        return false;
    }

    // RPC-010 FIX: Constant-time username comparison (prevents username enumeration)
    // Old code leaked username length via early return, allowing timing attacks
    // New code always performs full comparison regardless of length mismatch

    // Prepare username buffers with padding for constant-time comparison
    const size_t MAX_USERNAME_LEN = 256;
    uint8_t usernamePadded[MAX_USERNAME_LEN] = {0};
    uint8_t storedUserPadded[MAX_USERNAME_LEN] = {0};

    // Copy usernames to padded buffers (truncate if too long)
    size_t userLen = std::min(username.length(), MAX_USERNAME_LEN);
    size_t storedLen = std::min(g_rpcUser.length(), MAX_USERNAME_LEN);
    memcpy(usernamePadded, username.c_str(), userLen);
    memcpy(storedUserPadded, g_rpcUser.c_str(), storedLen);

    // Constant-time comparison of full buffers (including padding)
    // This prevents leaking username length information
    bool usernameMatch = SecureCompare(usernamePadded, storedUserPadded, MAX_USERNAME_LEN);

    // Also check lengths match (after constant-time buffer comparison)
    usernameMatch = usernameMatch && (username.length() == g_rpcUser.length());

    // RPC-022 FIX: Clear sensitive username buffers
    memset(usernamePadded, 0, MAX_USERNAME_LEN);
    memset(storedUserPadded, 0, MAX_USERNAME_LEN);

    // CRITICAL: Always verify password even if username wrong (prevent timing leak)
    // This ensures constant-time behavior regardless of username match
    bool passwordMatch = VerifyPassword(password, g_passwordSalt, g_passwordHash);

    // Only return true if BOTH username and password match
    return usernameMatch && passwordMatch;
}

bool SecureCompare(const uint8_t* a, const uint8_t* b, size_t len) {
    if (len == 0) return true;

    uint8_t result = 0;

    // XOR all bytes - if any differ, result will be non-zero
    for (size_t i = 0; i < len; i++) {
        result |= a[i] ^ b[i];
    }

    // Return true only if all bytes matched (result == 0)
    return result == 0;
}

CachedAuthResult TryCachedAuth(const std::string& username,
                               const std::string& password,
                               uint32_t& permissionsOut) {
    std::array<uint8_t, 32> key;
    DeriveCacheKey(username, password, key);

    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(g_authCacheMutex);
    EvictExpired(now);

    auto it = FindLive(key, now);
    if (it == g_authCache.end()) {
        // Wipe stack key before return.
        std::memset(key.data(), 0, key.size());
        return CachedAuthResult::Miss;
    }

    bool ok = it->success;
    if (ok) {
        permissionsOut = it->permissions;
    }

    std::memset(key.data(), 0, key.size());
    return ok ? CachedAuthResult::HitSuccess : CachedAuthResult::HitFail;
}

void CacheAuthSuccess(const std::string& username,
                      const std::string& password,
                      uint32_t permissions) {
    AuthCacheEntry e;
    DeriveCacheKey(username, password, e.key);
    e.expiry = std::chrono::steady_clock::now() +
               std::chrono::milliseconds(AUTH_CACHE_TTL_OK_MS);
    e.permissions = permissions;
    e.success = true;

    std::lock_guard<std::mutex> lock(g_authCacheMutex);
    // Drop any prior entry for this key first (avoid duplicates that would
    // delay eviction of the most-recently-verified entry).
    for (auto it = g_authCache.begin(); it != g_authCache.end(); ) {
        if (SecureCompare(it->key.data(), e.key.data(), 32)) {
            std::memset(it->key.data(), 0, 32);
            it = g_authCache.erase(it);
        } else {
            ++it;
        }
    }
    InsertEntry(e);
}

void CacheAuthFail(const std::string& username,
                   const std::string& password) {
    AuthCacheEntry e;
    DeriveCacheKey(username, password, e.key);
    e.expiry = std::chrono::steady_clock::now() +
               std::chrono::milliseconds(AUTH_CACHE_TTL_FAIL_MS);
    e.permissions = 0;
    e.success = false;

    std::lock_guard<std::mutex> lock(g_authCacheMutex);
    for (auto it = g_authCache.begin(); it != g_authCache.end(); ) {
        if (SecureCompare(it->key.data(), e.key.data(), 32)) {
            std::memset(it->key.data(), 0, 32);
            it = g_authCache.erase(it);
        } else {
            ++it;
        }
    }
    InsertEntry(e);
}

void ClearAuthCache() {
    std::lock_guard<std::mutex> lock(g_authCacheMutex);
    for (auto& e : g_authCache) {
        std::memset(e.key.data(), 0, 32);
    }
    g_authCache.clear();
}

} // namespace RPCAuth
