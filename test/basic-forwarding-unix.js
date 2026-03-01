const path = require('path');
const {
  spawnDaemon,
  killDaemon,
  killAllDaemons,
  connectUnixApi,
  apiCommand,
  createUdpEchoServer,
  sendUdp,
  TIMEOUT
} = require('./helpers');

const CONFIG_PATH = path.join(__dirname, 'config-unix.ini');
const API_SOCKET = '/tmp/udphole-test.sock';

async function runTest() {
  let daemon = null;
  let apiSock = null;
  let echoServer = null;
  let returnCode = 0;

  console.log('=== Basic Forwarding Test (Unix Socket) ===');
  console.log('Testing: UDP packets are forwarded from listen socket to connect socket\n');

  try {
    console.log('1. Spawning daemon...');
    daemon = await spawnDaemon(CONFIG_PATH);
    console.log(`   Daemon started (PID: ${daemon.pid})`);

    console.log('2. Connecting to API via Unix socket...');
    apiSock = await connectUnixApi(API_SOCKET);
    console.log('   Connected to API');

    console.log('3. Authenticating...');
    let resp = await apiCommand(apiSock, 'auth', 'finwo', 'testsecret');
    console.log(`   Auth response: ${resp}`);
    if (resp !== 'OK') throw new Error('Authentication failed');

    console.log('4. Creating session...');
    resp = await apiCommand(apiSock, 'session.create', 'test-session', '60');
    console.log(`   Session create: ${resp}`);

    console.log('5. Creating listen socket...');
    resp = await apiCommand(apiSock, 'session.socket.create.listen', 'test-session', 'client-a');
    const listenPort = resp[0];
    console.log(`   Listen socket port: ${listenPort}`);

    console.log('6. Starting echo server...');
    echoServer = await createUdpEchoServer();
    console.log(`   Echo server on port: ${echoServer.port}`);

    console.log('7. Creating connect socket to echo server...');
    resp = await apiCommand(apiSock, 'session.socket.create.connect', 'test-session', 'relay', '127.0.0.1', echoServer.port);
    console.log(`   Connect socket: ${resp}`);

    console.log('8. Creating forward: client-a -> relay...');
    resp = await apiCommand(apiSock, 'session.forward.create', 'test-session', 'client-a', 'relay');
    console.log(`   Forward create: ${resp}`);

    console.log('   Waiting for session to initialize...');
    await new Promise(r => setTimeout(r, 100));

    console.log('9. Sending UDP packet to listen socket...');
    await sendUdp(listenPort, '127.0.0.1', 'hello');
    console.log('   Sent "hello"');

    console.log('10. Waiting for echo response...');
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

    if (msg.data === 'hello') {
      console.log('\n✓ PASS: UDP forwarding works correctly (Unix socket)');
      console.log('   Packet was forwarded from listen socket to connect socket');
      console.log('   and echoed back successfully.');
    } else {
      throw new Error(`Expected "hello", got "${msg.data}"`);
    }

  } catch (err) {
    console.error(`\n✗ FAIL: ${err.message}`);
    returnCode = 1;
  } finally {
    if (echoServer) echoServer.socket.close();
    if (apiSock) apiSock.end();
    if (daemon) await killDaemon(daemon);
    await killAllDaemons();
    process.exit(returnCode);
  }
}

runTest();
