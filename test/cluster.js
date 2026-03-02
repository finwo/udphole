const path = require('path');
const {
  spawnDaemon,
  killDaemon,
  killAllDaemons,
  connectApi,
  apiCommand,
  findFreePort,
  sleep,
  createUdpEchoServer,
  sendUdp,
  TIMEOUT
} = require('./helpers');

const CLUSTER_CONFIG_PATH = path.join(__dirname, 'config-cluster.ini');
const NODE1_CONFIG_PATH = path.join(__dirname, 'config-node1.ini');
const NODE2_CONFIG_PATH = path.join(__dirname, 'config-node2.ini');
const CLUSTER_API_PORT = 19121;
const NODE1_API_PORT = 19122;
const NODE2_API_PORT = 19123;

async function runTest() {
  let cluster = null;
  let node1 = null;
  let node2 = null;
  let clusterApi = null;
  let node1Api = null;
  let node2Api = null;
  let returnCode = 0;
  let resp;

  console.log('=== Cluster Test ===\n');

  await killAllDaemons();

  try {
    console.log('1. Starting backing daemon node1...');
    node1 = await spawnDaemon(NODE1_CONFIG_PATH, 'daemon');
    console.log(`   Node1 started (PID: ${node1.pid})`);

    console.log('2. Starting backing daemon node2...');
    node2 = await spawnDaemon(NODE2_CONFIG_PATH, 'daemon');
    console.log(`   Node2 started (PID: ${node2.pid})`);

    console.log('3. Starting cluster...');
    cluster = await spawnDaemon(CLUSTER_CONFIG_PATH, 'cluster');
    console.log(`   Cluster started (PID: ${cluster.pid})`);

    await sleep(6000); // Wait for healthcheck to run

    console.log('4. Connecting to cluster API...');
    clusterApi = await connectApi(CLUSTER_API_PORT);
    console.log('   Connected to cluster');

    console.log('5. Connecting to node1 API...');
    node1Api = await connectApi(NODE1_API_PORT);
    console.log('   Connected to node1');
    resp = await apiCommand(node1Api, 'auth', 'finwo', 'testsecret');
    console.log(`   Auth response: ${resp}`);

    console.log('6. Connecting to node2 API...');
    node2Api = await connectApi(NODE2_API_PORT);
    console.log('   Connected to node2');
    resp = await apiCommand(node2Api, 'auth', 'finwo', 'testsecret');
    console.log(`   Auth response: ${resp}`);

    console.log('7. Authenticating with cluster...');
    resp = await apiCommand(clusterApi, 'auth', 'test', 'testsecret');
    console.log(`   Auth response: ${resp}`);
    if (resp !== 'OK') throw new Error('Cluster authentication failed');

    console.log('8. Testing session.count on cluster (no sessions)...');
    resp = await apiCommand(clusterApi, 'session.count');
    console.log(`   cluster session.count: ${resp}`);
    if (typeof resp !== 'number' || resp !== 0) {
      throw new Error(`Expected 0 sessions, got ${resp}`);
    }

    console.log('9. Creating session on cluster...');
    resp = await apiCommand(clusterApi, 'session.create', 'test-session-1', '60');
    console.log(`   session.create: ${resp}`);
    if (resp !== 'OK') throw new Error('Failed to create session via cluster');

    console.log('10. Verifying session created on one of the nodes...');
    let node1Count = await apiCommand(node1Api, 'session.count');
    let node2Count = await apiCommand(node2Api, 'session.count');
    console.log(`    Node1 session.count: ${node1Count}, Node2 session.count: ${node2Count}`);
    let totalCount = node1Count + node2Count;
    if (totalCount !== 1) {
      throw new Error(`Expected 1 total session across nodes, got ${totalCount}`);
    }

    console.log('11. Testing session.list aggregation...');
    resp = await apiCommand(clusterApi, 'session.list');
    console.log(`    cluster session.list: ${JSON.stringify(resp)}`);
    if (!Array.isArray(resp) || resp.length !== 1) {
      throw new Error(`Expected 1 session in list, got ${resp.length}`);
    }

    console.log('12. Testing session.count aggregation...');
    resp = await apiCommand(clusterApi, 'session.count');
    console.log(`    cluster session.count: ${resp}`);
    if (resp !== totalCount) {
      throw new Error(`Expected ${totalCount} sessions, got ${resp}`);
    }

    console.log('13. Creating another session (should go to different node)...');
    resp = await apiCommand(clusterApi, 'session.create', 'test-session-2', '60');
    console.log(`    session.create: ${resp}`);
    if (resp !== 'OK') throw new Error('Failed to create second session');

    console.log('14. Verifying sessions distributed...');
    node1Count = await apiCommand(node1Api, 'session.count');
    node2Count = await apiCommand(node2Api, 'session.count');
    console.log(`    Node1: ${node1Count}, Node2: ${node2Count}`);
    if (node1Count + node2Count !== 2) {
      throw new Error('Expected 2 total sessions');
    }

    console.log('15. Testing session.info routing...');
    resp = await apiCommand(clusterApi, 'session.info', 'test-session-1');
    console.log(`    session.info: ${JSON.stringify(resp).substring(0, 80)}...`);
    if (!Array.isArray(resp)) {
      throw new Error('Expected array response for session.info');
    }

    console.log('16. Testing session.destroy...');
    resp = await apiCommand(clusterApi, 'session.destroy', 'test-session-1');
    console.log(`    session.destroy: ${resp}`);
    if (resp !== 'OK') throw new Error('Failed to destroy session');

    console.log('17. Verifying session destroyed...');
    resp = await apiCommand(clusterApi, 'session.count');
    console.log(`    cluster session.count after destroy: ${resp}`);
    if (resp !== 1) {
      throw new Error(`Expected 1 session after destroy, got ${resp}`);
    }

    console.log('18. Testing socket creation on existing session...');
    resp = await apiCommand(clusterApi, 'session.socket.create.listen', 'test-session-2', 'socket1');
    console.log(`    socket.create.listen: ${JSON.stringify(resp)}`);
    if (!Array.isArray(resp) || resp.length < 1) {
      throw new Error('Expected port in response');
    }

    console.log('18b. Testing advertise address and UDP forwarding...');
    const listenPort = resp[0];
    const advertiseAddr = resp[1];
    console.log(`    Listen port: ${listenPort}, advertise addr: ${advertiseAddr}`);
    if (advertiseAddr !== '127.0.0.2' && advertiseAddr !== '127.0.0.3') {
      throw new Error(`Expected advertise address 127.0.0.2 or 127.0.0.3, got ${advertiseAddr}`);
    }

    console.log('    Starting echo server...');
    const echoServer = await createUdpEchoServer();
    console.log(`    Echo server on port: ${echoServer.port}`);

    console.log('    Creating connect socket to echo server...');
    resp = await apiCommand(clusterApi, 'session.socket.create.connect', 'test-session-2', 'relay', '127.0.0.1', echoServer.port);
    console.log(`    socket.create.connect: ${JSON.stringify(resp)}`);

    console.log('    Creating forward: socket1 -> relay...');
    resp = await apiCommand(clusterApi, 'session.forward.create', 'test-session-2', 'socket1', 'relay');
    console.log(`    forward.create: ${resp}`);

    console.log('    Sending UDP packet to listen socket...');
    await sendUdp(listenPort, advertiseAddr, 'hello');
    console.log('    Sent "hello"');

    console.log('    Waiting for echo response...');
    const messages = echoServer.getMessages();
    const start = Date.now();
    while (messages.length === 0 && Date.now() - start < TIMEOUT) {
      await new Promise(r => setTimeout(r, 50));
    }
    if (messages.length === 0) {
      throw new Error('Timeout: no message received by echo server');
    }
    const msg = messages[0];
    console.log(`    Received: "${msg.data}" from ${msg.rinfo.address}:${msg.rinfo.port}`);
    if (msg.data !== 'hello') {
      throw new Error(`Expected "hello", got "${msg.data}"`);
    }
    echoServer.socket.close();

    console.log('19. Testing node failure handling...');
    console.log('    Killing node1...');
    await killDaemon(node1);
    node1Api.end();
    node1Api = null;
    await sleep(6000); // Wait for healthcheck to detect failure

    console.log('    Creating session while node1 is down...');
    resp = await apiCommand(clusterApi, 'session.create', 'test-session-3', '60');
    console.log(`    session.create: ${resp}`);
    if (resp !== 'OK') throw new Error('Failed to create session while node1 down');

    console.log('    Verifying session via cluster session.list...');
    resp = await apiCommand(clusterApi, 'session.list');
    console.log('    cluster session.list:', resp);
    if (!Array.isArray(resp) || resp.length < 1) {
      throw new Error('Expected at least 1 session in cluster');
    }

    console.log('    Restarting node1...');
    node1 = await spawnDaemon(NODE1_CONFIG_PATH, 'daemon');
    console.log(`    Node1 restarted (PID: ${node1.pid})`);
    await sleep(1000);
    node1Api = await connectApi(NODE1_API_PORT);
    await apiCommand(node1Api, 'auth', 'finwo', 'testsecret');
    await sleep(6000); // Wait for healthcheck to detect recovery

    console.log('    Creating another session after node1 recovery...');
    resp = await apiCommand(clusterApi, 'session.create', 'test-session-4', '60');
    console.log(`    session.create: ${resp}`);
    if (resp !== 'OK') throw new Error('Failed to create session after node1 recovery');

    console.log('\n✓ PASS: All cluster tests passed');

  } catch (err) {
    console.error(`\n✗ FAIL: ${err.message}`);
    console.error(err.stack);
    returnCode = 1;
  } finally {
    if (clusterApi) clusterApi.end();
    if (node1Api) node1Api.end();
    if (node2Api) node2Api.end();
    if (cluster) await killDaemon(cluster);
    if (node1) await killDaemon(node1);
    if (node2) await killDaemon(node2);
    await killAllDaemons();
    process.exit(returnCode);
  }
}

runTest();
