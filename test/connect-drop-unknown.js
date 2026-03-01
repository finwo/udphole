const path = require('path');
const dgram = require('dgram');
const {
  spawnDaemon,
  killDaemon,
  killAllDaemons,
  connectApi,
  apiCommand,
  createUdpEchoServer,
  TIMEOUT
} = require('./helpers');

const CONFIG_PATH = path.join(__dirname, 'config-tcp.ini');
const API_PORT = 9123;

function sendAndRecvUdp(srcPort, dstPort, message) {
  return new Promise((resolve, reject) => {
    const sock = dgram.createSocket('udp4');
    let resolved = false;
    
    const timer = setTimeout(() => {
      if (!resolved) {
        resolved = true;
        sock.close();
        resolve(null);
      }
    }, TIMEOUT);
    
    sock.on('message', (msg, rinfo) => {
      if (!resolved) {
        resolved = true;
        clearTimeout(timer);
        sock.close();
        resolve({ data: msg.toString(), rinfo });
      }
    });
    
    sock.bind(srcPort, '127.0.0.1', () => {
      const buf = Buffer.from(message);
      sock.send(buf, 0, buf.length, dstPort, '127.0.0.1', (err) => {
        if (err && !resolved) {
          resolved = true;
          clearTimeout(timer);
          sock.close();
          reject(err);
        }
      });
    });
    
    sock.on('error', (err) => {
      if (!resolved) {
        resolved = true;
        clearTimeout(timer);
        sock.close();
        reject(err);
      }
    });
  });
}

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
  let returnCode = 0;

  console.log('=== Connect Socket Drop Unknown Test ===');
  console.log('Testing: connect mode sockets drop traffic from unknown remote addresses\n');

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
    resp = await apiCommand(apiSock, 'session.create', 'test-connect-drop', '60');
    console.log(`   Session create: ${resp}`);

    console.log('5. Creating listen socket...');
    resp = await apiCommand(apiSock, 'session.socket.create.listen', 'test-connect-drop', 'listen');
    const listenPort = resp[0];
    console.log(`   Listen socket port: ${listenPort}`);

    console.log('6. Starting echo server...');
    echoServer = await createUdpEchoServer();
    console.log(`   Echo server on port: ${echoServer.port}`);

    console.log('7. Creating connect socket to echo server...');
    resp = await apiCommand(apiSock, 'session.socket.create.connect', 'test-connect-drop', 'connect', '127.0.0.1', echoServer.port);
    const connectPort = resp[0];
    console.log(`   Connect socket local port: ${connectPort}`);

    console.log('8. Creating forward: listen -> connect...');
    resp = await apiCommand(apiSock, 'session.forward.create', 'test-connect-drop', 'listen', 'connect');
    console.log(`   Forward create: ${resp}`);

    console.log('9. Creating forward: connect -> listen (for echo responses)...');
    resp = await apiCommand(apiSock, 'session.forward.create', 'test-connect-drop', 'connect', 'listen');
    console.log(`   Forward create: ${resp}`);

    console.log('10. Waiting for session to initialize...');
    await new Promise(r => setTimeout(r, 100));

    console.log('\n=== Step 1: Happy path - active to listen, through proxy, back to active ===');
    console.log('11. Sending "step1-test" from port 50001 to listen socket...');

    const recvPromise1 = sendAndRecvUdp(50001, listenPort, 'step1-test');
    console.log('    Sent "step1-test", waiting for response...');

    let messages = echoServer.getMessages();
    let start = Date.now();
    while (messages.length === 0 && Date.now() - start < TIMEOUT) {
      await new Promise(r => setTimeout(r, 50));
      messages = echoServer.getMessages();
    }

    if (messages.length === 0) {
      throw new Error('Timeout: no message received by echo server in step 1');
    }

    const msgStep1 = messages[0];
    console.log(`    Echo server received: "${msgStep1.data}" from ${msgStep1.rinfo.address}:${msgStep1.rinfo.port}`);

    if (msgStep1.data !== 'step1-test') {
      throw new Error(`Expected "step1-test", got "${msgStep1.data}"`);
    }
    console.log('    ✓ Echo server received the packet');

    echoServer.clearMessages();

    const responseToSender1 = await recvPromise1;
    if (responseToSender1 === null) {
      throw new Error('Timeout: no response received back at sender port 50001');
    }
    console.log(`    ✓ Sender received echo response: "${responseToSender1.data}" from ${responseToSender1.rinfo.address}:${responseToSender1.rinfo.port}`);

    if (responseToSender1.data !== 'step1-test') {
      throw new Error(`Expected echo "step1-test", got "${responseToSender1.data}"`);
    }

    console.log('\n=== Step 2: Verify connect socket drops unknown source ===');
    console.log('12. Sending "step2-direct" directly to connect socket from port 50002...');

    const recvPromise2 = sendAndRecvUdp(50002, connectPort, 'step2-direct');
    console.log('    Sent "step2-direct", waiting for response...');

    messages = echoServer.getMessages();
    start = Date.now();
    while (Date.now() - start < 500) {
      await new Promise(r => setTimeout(r, 50));
      messages = echoServer.getMessages();
    }

    if (messages.length > 0) {
      console.log(`    ✗ FAIL: Echo server received packet from connect socket (should have been dropped)`);
      console.log(`      Received: "${messages[0].data}" from ${messages[0].rinfo.address}:${messages[0].rinfo.port}`);
      throw new Error('Connect socket did not drop traffic from unknown source');
    }
    console.log('    ✓ Echo server did NOT receive the packet (correctly dropped)');

    const responseToSender2 = await recvPromise2;
    if (responseToSender2 !== null) {
      console.log(`    ✗ FAIL: Sender received response from connect socket (should have been dropped)`);
      console.log(`      Received: "${responseToSender2.data}" from ${responseToSender2.rinfo.address}:${responseToSender2.rinfo.port}`);
      throw new Error('Connect socket did not drop traffic - response sent back to sender');
    }
    console.log('    ✓ Sender did NOT receive any response (correctly dropped)');
    console.log('\n✓ PASS: Connect socket correctly dropped traffic from unknown source');

    console.log('\n=== Step 3: Verify listen->connect forward still works ===');
    echoServer.clearMessages();

    console.log('13. Sending "step3-retry" from port 50001 to listen socket again...');

    const recvPromise3 = sendAndRecvUdp(50001, listenPort, 'step3-retry');
    console.log('    Sent "step3-retry", waiting for response...');

    messages = echoServer.getMessages();
    start = Date.now();
    while (messages.length === 0 && Date.now() - start < TIMEOUT) {
      await new Promise(r => setTimeout(r, 50));
      messages = echoServer.getMessages();
    }

    if (messages.length === 0) {
      throw new Error('Timeout: no message received by echo server in step 3');
    }

    const msgStep3 = messages[0];
    console.log(`    Echo server received: "${msgStep3.data}" from ${msgStep3.rinfo.address}:${msgStep3.rinfo.port}`);

    if (msgStep3.data !== 'step3-retry') {
      throw new Error(`Expected "step3-retry", got "${msgStep3.data}"`);
    }
    console.log('    ✓ Echo server received the packet');

    echoServer.clearMessages();

    const responseToSender3 = await recvPromise3;
    if (responseToSender3 === null) {
      throw new Error('Timeout: no response received back at sender port 50001 in step 3');
    }
    console.log(`    ✓ Sender received echo response: "${responseToSender3.data}" from ${responseToSender3.rinfo.address}:${responseToSender3.rinfo.port}`);

    if (responseToSender3.data !== 'step3-retry') {
      throw new Error(`Expected echo "step3-retry", got "${responseToSender3.data}"`);
    }

    console.log('\n✓ PASS: Connect socket drop unknown test completed successfully');

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
