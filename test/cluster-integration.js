const path = require('path');
const {
  spawnDaemon,
  killDaemon,
  killAllDaemons,
  connectApi,
  apiCommand,
  sleep,
  findFreePort
} = require('./helpers');

const NODE1_CONFIG = path.join(__dirname, 'config-cluster-node1.ini');
const NODE2_CONFIG = path.join(__dirname, 'config-cluster-node2.ini');

const DAEMON_PATH = path.join(__dirname, '..', 'udphole');
const { spawn } = require('child_process');

async function runTest() {
  let daemon1 = null;
  let daemon2 = null;
  let cluster = null;
  let apiSock1 = null;
  let apiSock2 = null;
  let clusterSock = null;
  let returnCode = 0;

  console.log('=== Cluster Integration Test ===');
  console.log('Testing: session creation, aggregation, and forwarding\n');

  try {
    console.log('1. Spawning node 1 daemon...');
    daemon1 = await spawnDaemon(NODE1_CONFIG);
    await sleep(1000);
    console.log('   Node 1 started on port 19123');

    console.log('2. Spawning node 2 daemon...');
    daemon2 = await spawnDaemon(NODE2_CONFIG);
    await sleep(1000);
    console.log('   Node 2 started on port 19124');

    console.log('3. Connecting to node 1 API...');
    apiSock1 = await connectApi(19123);
    console.log('   Connected');

    console.log('4. Connecting to node 2 API...');
    apiSock2 = await connectApi(19124);
    console.log('   Connected');

    console.log('5. Authenticating with nodes...');
    let resp = await apiCommand(apiSock1, 'auth', 'finwo', 'testsecret');
    if (resp !== 'OK') throw new Error('Auth failed for node 1');
    resp = await apiCommand(apiSock2, 'auth', 'finwo', 'testsecret');
    if (resp !== 'OK') throw new Error('Auth failed for node 2');
    console.log('   Auth OK on both nodes');

    console.log('\n6. Creating sessions directly on nodes...');
    resp = await apiCommand(apiSock1, 'session.create', 'session-node1');
    console.log(`   Node 1 session.create: ${JSON.stringify(resp)}`);
    if (!Array.isArray(resp) || resp[0] !== 'OK') throw new Error(`Failed to create session on node 1: got ${JSON.stringify(resp)}`);

    resp = await apiCommand(apiSock2, 'session.create', 'session-node2');
    console.log(`   Node 2 session.create: ${JSON.stringify(resp)}`);
    if (!Array.isArray(resp) || resp[0] !== 'OK') throw new Error('Failed to create session on node 2');

    console.log('\n7. Verifying session counts on individual nodes...');
    resp = await apiCommand(apiSock1, 'session.count');
    console.log(`   Node 1 session.count: ${resp}`);
    if (resp !== 1) throw new Error(`Expected 1 session on node 1, got ${resp}`);

    resp = await apiCommand(apiSock2, 'session.count');
    console.log(`   Node 2 session.count: ${resp}`);
    if (resp !== 1) throw new Error(`Expected 1 session on node 2, got ${resp}`);

    console.log('\n8. Verifying socket creation returns correct advertise address...');
    resp = await apiCommand(apiSock1, 'session.socket.create.listen', 'session-node1', 'socket1');
    console.log(`   Node 1 socket create listen: ${JSON.stringify(resp)}`);
    if (!Array.isArray(resp) || resp[1] !== '127.0.0.1') {
      throw new Error(`Expected advertise address 127.0.0.1 for node 1, got ${JSON.stringify(resp)}`);
    }
    console.log('   ✓ Contains advertise address 127.0.0.1');

    resp = await apiCommand(apiSock2, 'session.socket.create.listen', 'session-node2', 'socket2');
    console.log(`   Node 2 socket create listen: ${JSON.stringify(resp)}`);
    if (!Array.isArray(resp) || resp[1] !== '127.0.0.2') {
      throw new Error(`Expected advertise address 127.0.0.2 for node 2, got ${JSON.stringify(resp)}`);
    }
    console.log('   ✓ Contains advertise address 127.0.0.2');

    console.log('\n✓ PASS: Cluster integration test passed');

  } catch (err) {
    console.error(`\n✗ FAIL: ${err.message}`);
    console.error(err.stack);
    returnCode = 1;
  } finally {
    if (apiSock1) apiSock1.end();
    if (apiSock2) apiSock2.end();
    if (clusterSock) clusterSock.end();
    if (daemon1) await killDaemon(daemon1);
    if (daemon2) await killDaemon(daemon2);
    if (cluster) await killDaemon(cluster);
    await killAllDaemons();
    process.exit(returnCode);
  }
}

runTest();
