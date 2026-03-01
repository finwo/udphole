const path = require('path');
const dgram = require('dgram');
const {
  spawnDaemon,
  killDaemon,
  connectApi,
  apiCommand,
  createUdpEchoServer,
  TIMEOUT
} = require('./helpers');

const CONFIG_PATH = path.join(__dirname, 'config-tcp.ini');
const API_PORT = 9123;

function sendUdpFromPort(srcPort, dstPort, message) {
  return new Promise((resolve, reject) => {
    const sock = dgram.createSocket('udp4');
    sock.bind(srcPort, '127.0.0.1', () => {
      const buf = Buffer.from(message);
      sock.send(buf, 0, buf.length, dstPort, '127.0.0.1', (err) => {
        if (err) {
          sock.close();
          reject(err);
        } else {
          setTimeout(() => {
            sock.close();
            resolve();
          }, 50);
        }
      });
    });
    sock.on('error', reject);
  });
}

async function runTest() {
  let daemon = null;
  let apiSock = null;
  let echoServer = null;

  console.log('=== Listen Socket Re-learn Test ===');
  console.log('Testing: listen socket re-learns remote address when different client sends\n');

  try {
    console.log('1. Spawning daemon...');
    daemon = await spawnDaemon(CONFIG_PATH);
    console.log(`   Daemon started (PID: ${daemon.pid})`);

    console.log('2. Connecting to API...');
    apiSock = await connectApi(API_PORT);
    console.log('   Connected to API');

    console.log('3. Authenticating...');
    let resp = await apiCommand(apiSock, 'auth', 'finwo', 'testsecret');
    console.log(`   Auth response: ${resp}`);
    if (resp !== 'OK') throw new Error('Authentication failed');

    console.log('4. Creating session...');
    resp = await apiCommand(apiSock, 'session.create', 'test-relearn', '60');
    console.log(`   Session create: ${resp}`);

    console.log('5. Creating listen socket...');
    resp = await apiCommand(apiSock, 'session.socket.create.listen', 'test-relearn', 'listener');
    const listenPort = resp[0];
    console.log(`   Listen socket port: ${listenPort}`);

    console.log('6. Starting echo server...');
    echoServer = await createUdpEchoServer();
    console.log(`   Echo server on port: ${echoServer.port}`);

    console.log('7. Creating connect socket to echo server...');
    resp = await apiCommand(apiSock, 'session.socket.create.connect', 'test-relearn', 'relay', '127.0.0.1', echoServer.port);
    console.log(`   Connect socket: ${resp}`);

    console.log('8. Creating forward: listener -> relay...');
    resp = await apiCommand(apiSock, 'session.forward.create', 'test-relearn', 'listener', 'relay');
    console.log(`   Forward create: ${resp}`);

    console.log('9. First client sending "from-A" from port 50001...');
    await sendUdpFromPort(50001, listenPort, 'from-A');
    console.log('   Sent "from-A"');

    let messages = echoServer.getMessages();
    let start = Date.now();
    while (messages.length === 0 && Date.now() - start < TIMEOUT) {
      await new Promise(r => setTimeout(r, 50));
      messages = echoServer.getMessages();
    }

    if (messages.length === 0) {
      throw new Error('Timeout: no message received from first client');
    }

    const msgA = messages[0];
    console.log(`    Received: "${msgA.data}" from ${msgA.rinfo.address}:${msgA.rinfo.port}`);

    if (msgA.data !== 'from-A') {
      throw new Error(`Expected "from-A", got "${msgA.data}"`);
    }
    console.log('   ✓ First client connection established, listen socket learned address');

    echoServer.clearMessages();

    console.log('10. Second client sending "from-B" from port 50002...');
    await sendUdpFromPort(50002, listenPort, 'from-B');
    console.log('   Sent "from-B"');

    messages = echoServer.getMessages();
    start = Date.now();
    while (messages.length === 0 && Date.now() - start < TIMEOUT) {
      await new Promise(r => setTimeout(r, 50));
      messages = echoServer.getMessages();
    }

    if (messages.length === 0) {
      throw new Error('Timeout: no message received from second client');
    }

    const msgB = messages[0];
    console.log(`    Received: "${msgB.data}" from ${msgB.rinfo.address}:${msgB.rinfo.port}`);

    if (msgB.data === 'from-B') {
      console.log('\n✓ PASS: Listen socket correctly re-learned new remote address');
      console.log('   Second client (from port 50002) was able to communicate');
      console.log('   through the same listen socket after the first client.');
      process.exit(0);
    } else if (msgB.data === 'from-A') {
      console.log('\n✗ FAIL: Listen socket did NOT re-learn new remote address');
      console.log('   The second client\'s packet was sent back to the first client');
      console.log('   instead of the second client. This is the bug to fix.');
      console.log(`   Expected to receive from port 50002, but responses went to port ${msgA.rinfo.port}`);
      process.exit(1);
    } else {
      throw new Error(`Unexpected message: "${msgB.data}"`);
    }

  } catch (err) {
    console.error(`\n✗ FAIL: ${err.message}`);
    process.exit(1);
  } finally {
    if (echoServer) echoServer.socket.close();
    if (apiSock) apiSock.end();
    if (daemon) await killDaemon(daemon);
  }
}

runTest();