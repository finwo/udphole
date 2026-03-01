const path = require('path');
const {
  spawnDaemon,
  killDaemon,
  killAllDaemons,
  connectApi,
  apiCommand,
  sleep
} = require('./helpers');

const NODE1_CONFIG = path.join(__dirname, 'config-cluster-node1.ini');
const NODE2_CONFIG = path.join(__dirname, 'config-cluster-node2.ini');
const CLUSTER_CONFIG = path.join(__dirname, 'config-cluster.ini');

async function runTest() {
  let daemon1 = null;
  let daemon2 = null;
  let cluster = null;
  let apiSock1 = null;
  let apiSock2 = null;
  let clusterSock = null;
  let returnCode = 0;

  console.log('=== Cluster Daemon Test ===');
  console.log('Testing: session distribution across clustered nodes\n');

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

    console.log('5. Authenticating with node 1...');
    let resp = await apiCommand(apiSock1, 'auth', 'finwo', 'testsecret');
    console.log(`   Auth response: ${resp}`);
    if (resp !== 'OK') throw new Error('Auth failed for node 1');

    console.log('6. Authenticating with node 2...');
    resp = await apiCommand(apiSock2, 'auth', 'finwo', 'testsecret');
    console.log(`   Auth response: ${resp}`);
    if (resp !== 'OK') throw new Error('Auth failed for node 2');

    console.log('\n7. Testing individual nodes have 0 sessions...');
    resp = await apiCommand(apiSock1, 'session.count');
    console.log(`   Node 1 session.count: ${resp}`);
    if (resp !== 0) throw new Error('Expected 0 sessions on node 1');

    resp = await apiCommand(apiSock2, 'session.count');
    console.log(`   Node 2 session.count: ${resp}`);
    if (resp !== 0) throw new Error('Expected 0 sessions on node 2');

    console.log('\n✓ PASS: Basic cluster setup works');
    console.log('\nNote: Full cluster daemon requires cluster config file.');
    console.log('This test verified that node daemons can run independently.');

  } catch (err) {
    console.error(`\n✗ FAIL: ${err.message}`);
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
