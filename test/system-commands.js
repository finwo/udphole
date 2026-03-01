const path = require('path');
const {
  spawnDaemon,
  killDaemon,
  connectApi,
  apiCommand
} = require('./helpers');

const CONFIG_PATH = path.join(__dirname, 'config-tcp.ini');
const API_PORT = 9123;

async function runTest() {
  let daemon = null;
  let apiSock = null;

  console.log('=== System Commands Test ===');
  console.log('Testing: system.load and session.count commands\n');

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

    console.log('4. Testing session.count (no sessions)...');
    resp = await apiCommand(apiSock, 'session.count');
    console.log(`   session.count: ${resp}`);
    if (typeof resp !== 'number' || resp !== 0) {
      throw new Error(`Expected session.count = 0, got ${resp}`);
    }

    console.log('5. Creating a session...');
    resp = await apiCommand(apiSock, 'session.create', 'test-session', '60');
    console.log(`   Session create: ${resp}`);

    console.log('6. Testing session.count (with 1 session)...');
    resp = await apiCommand(apiSock, 'session.count');
    console.log(`   session.count: ${resp}`);
    if (typeof resp !== 'number' || resp !== 1) {
      throw new Error(`Expected session.count = 1, got ${resp}`);
    }

    console.log('7. Creating another session...');
    resp = await apiCommand(apiSock, 'session.create', 'test-session-2', '60');
    console.log(`   Session create: ${resp}`);

    console.log('8. Testing session.count (with 2 sessions)...');
    resp = await apiCommand(apiSock, 'session.count');
    console.log(`   session.count: ${resp}`);
    if (typeof resp !== 'number' || resp !== 2) {
      throw new Error(`Expected session.count = 2, got ${resp}`);
    }

    console.log('9. Testing system.load...');
    resp = await apiCommand(apiSock, 'system.load');
    console.log(`   system.load: ${JSON.stringify(resp)}`);
    if (!Array.isArray(resp) || resp.length !== 6) {
      throw new Error(`Expected array with 6 elements, got ${JSON.stringify(resp)}`);
    }
    if (resp[0] !== '1min' || resp[2] !== '5min' || resp[4] !== '15min') {
      throw new Error(`Expected keys [1min, 5min, 15min], got ${JSON.stringify(resp)}`);
    }
    const load1 = parseFloat(resp[1]);
    const load5 = parseFloat(resp[3]);
    const load15 = parseFloat(resp[5]);
    console.log(`   Parsed loads: 1min=${load1}, 5min=${load5}, 15min=${load15}`);
    if (isNaN(load1) || isNaN(load5) || isNaN(load15)) {
      throw new Error('Load values should be valid numbers');
    }

    console.log('10. Destroying a session...');
    resp = await apiCommand(apiSock, 'session.destroy', 'test-session');
    console.log(`   Session destroy: ${resp}`);

    console.log('11. Testing session.count (after destroy)...');
    resp = await apiCommand(apiSock, 'session.count');
    console.log(`   session.count: ${resp}`);
    if (typeof resp !== 'number' || resp !== 1) {
      throw new Error(`Expected session.count = 1, got ${resp}`);
    }

    console.log('\n✓ PASS: All system commands work correctly');
    process.exit(0);

  } catch (err) {
    console.error(`\n✗ FAIL: ${err.message}`);
    process.exit(1);
  } finally {
    if (apiSock) apiSock.end();
    if (daemon) await killDaemon(daemon);
  }
}

runTest();
