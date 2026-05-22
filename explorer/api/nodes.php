<?php
/**
 * Block Explorer API - Seed Node Status (Live)
 *
 * Queries all 4 mainnet seed nodes directly via RPC.
 * NYC is local (127.0.0.1), others via direct HTTP.
 * Cached for 3 seconds to avoid overloading nodes.
 *
 * 2026-05-22: replaced 3 parallel per-node curl handles (12 HTTP requests
 * total) with one JSON-RPC batch per node (4 HTTP requests total). The
 * seed's per-IP token-bucket rate limiter costs 1 token per HTTP request,
 * so the old pattern drained 3 tokens per seed per Explorer poll, which
 * intermittently rejected one of the 3 calls (often getnetworkinfo or
 * getblockchaininfo) and produced ghost "offline" flashes in the UI.
 * Batch RPC: 1 HTTP request → 1 token → 1 auth check → 3 methods executed
 * server-side. Auth is also cache-warm on the cache-hit path (v4.4.2+leakfix).
 */

require_once __DIR__ . "/rpc.php";

$config = getChainConfig();
$chain = $config['chain'];
$rpcPort = $config['rpc_port'];

// 3-second cache
$cacheFile = __DIR__ . "/../cache/nodes-{$chain}.json";
if (file_exists($cacheFile)) {
    $cacheAge = time() - filemtime($cacheFile);
    if ($cacheAge < 3) {
        $cached = file_get_contents($cacheFile);
        if ($cached !== false) {
            $data = json_decode($cached, true);
            if ($data !== null) {
                $data['cached'] = true;
                $data['cacheAge'] = $cacheAge;
                sendJSON($data);
            }
        }
    }
}

$seedNodes = [
    ['id' => 'nyc', 'ip' => '138.197.68.128', 'host' => '127.0.0.1',      'label' => 'New York',  'flag' => 'US', 'primary' => true],
    ['id' => 'ldn', 'ip' => '167.172.56.119', 'host' => '167.172.56.119', 'label' => 'London',    'flag' => 'GB', 'primary' => false],
    ['id' => 'sgp', 'ip' => '165.22.103.114', 'host' => '165.22.103.114', 'label' => 'Singapore', 'flag' => 'SG', 'primary' => false],
    ['id' => 'syd', 'ip' => '134.199.159.83', 'host' => '134.199.159.83', 'label' => 'Sydney',    'flag' => 'AU', 'primary' => false],
];

// Method id mapping for batch responses (id field on each sub-request).
$batchMethods = [
    1 => 'getblockchaininfo',
    2 => 'getconnectioncount',
    3 => 'getnetworkinfo',
];

// Build one curl handle per node carrying a JSON-RPC batch of all 3 methods.
$multiHandle = curl_multi_init();
$curlHandles = [];

foreach ($seedNodes as $i => $node) {
    $batch = [];
    foreach ($batchMethods as $id => $method) {
        $batch[] = ['jsonrpc' => '2.0', 'id' => $id, 'method' => $method, 'params' => []];
    }
    $payload = json_encode($batch);

    $ch = curl_init();
    $url = "http://{$node['host']}:{$rpcPort}/";
    curl_setopt($ch, CURLOPT_URL, $url);
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_POST, true);
    curl_setopt($ch, CURLOPT_POSTFIELDS, $payload);
    curl_setopt($ch, CURLOPT_TIMEOUT, 10);
    curl_setopt($ch, CURLOPT_CONNECTTIMEOUT, 5);
    curl_setopt($ch, CURLOPT_HTTPHEADER, [
        'Content-Type: application/json',
        'X-Dilithion-RPC: 1',
        'Authorization: Basic ' . base64_encode('rpc:rpc'),
    ]);
    curl_multi_add_handle($multiHandle, $ch);
    $curlHandles[$i] = $ch;
}

$running = null;
do {
    curl_multi_exec($multiHandle, $running);
    curl_multi_select($multiHandle, 0.1);
} while ($running > 0);

$nodeData = [];
foreach ($seedNodes as $i => $node) {
    $nodeData[$i] = [
        'id'      => $node['id'],
        'ip'      => $node['ip'],
        'label'   => $node['label'],
        'flag'    => $node['flag'],
        'primary' => $node['primary'],
        'online'  => false,
        'height'  => null,
        'peers'   => null,
        'chain'   => null,
        'version' => null,
        'difficulty' => null,
    ];
}

foreach ($curlHandles as $i => $ch) {
    $response = curl_multi_getcontent($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_multi_remove_handle($multiHandle, $ch);
    curl_close($ch);

    if ($httpCode !== 200 || !$response) continue;
    $parsed = json_decode($response, true);
    if (!is_array($parsed)) continue;

    // JSON-RPC batch response is an array; single-response shape is also
    // tolerated (defensive — should not happen here, but the server may
    // return a single error object on a malformed batch).
    $entries = (isset($parsed[0]) && is_array($parsed[0])) ? $parsed : [$parsed];

    foreach ($entries as $entry) {
        if (!is_array($entry)) continue;
        if (isset($entry['error']) && $entry['error'] !== null) continue;
        $id = $entry['id'] ?? null;
        $result = $entry['result'] ?? null;
        if ($result === null || !isset($batchMethods[$id])) continue;

        $method = $batchMethods[$id];
        if ($method === 'getblockchaininfo') {
            $nodeData[$i]['online'] = true;
            $nodeData[$i]['height'] = $result['blocks'] ?? 0;
            $nodeData[$i]['chain'] = $result['chain'] ?? null;
            $nodeData[$i]['difficulty'] = $result['difficulty'] ?? null;
        } elseif ($method === 'getconnectioncount') {
            $nodeData[$i]['peers'] = $result;
        } elseif ($method === 'getnetworkinfo') {
            $nodeData[$i]['version'] = $result['subversion'] ?? null;
        }
    }
}

curl_multi_close($multiHandle);

$nodes = array_values($nodeData);
$heights = array_filter(array_column($nodes, 'height'), fn($h) => $h !== null);
$consensusHeight = !empty($heights) ? max($heights) : 0;

$response = [
    'nodes' => $nodes,
    'chain' => $chain,
    'consensusHeight' => $consensusHeight,
    'timestamp' => time(),
];

@file_put_contents($cacheFile, json_encode($response));
sendJSON($response);
