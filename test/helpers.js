const { spawn } = require('child_process');
const net = require('net');
const dgram = require('dgram');
const path = require('path');

const DAEMON_PATH = path.join(__dirname, '..', 'udphole');
const TIMEOUT = 2000;

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

function findFreePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.unref();
    server.on('error', reject);
    server.listen(0, () => {
      const addr = server.address();
      server.close(() => resolve(addr.port));
    });
  });
}

function spawnDaemon(configPath) {
  return new Promise(async (resolve, reject) => {
    await sleep(500);
    const daemon = spawn(DAEMON_PATH, ['-f', configPath, 'daemon', '-D'], {
      stdio: ['ignore', 'pipe', 'pipe', 'pipe']
    });

    let output = '';
    const startTimeout = setTimeout(() => {
      reject(new Error(`Daemon start timeout. Output: ${output}`));
    }, TIMEOUT);

    daemon.stderr.on('data', (data) => {
      process.stderr.write(data.toString());
      output += data.toString();
      if (output.includes('daemon started')) {
        clearTimeout(startTimeout);
        sleep(200).then(() => resolve(daemon));
      }
    });

    daemon.on('error', (err) => {
      clearTimeout(startTimeout);
      reject(err);
    });
  });
}

function killAllDaemons() {
  return new Promise((resolve) => {
    const { execSync } = require('child_process');
    try { execSync('pkill -9 udphole 2>/dev/null', { stdio: 'ignore' }); } catch(e) {}
    sleep(1000).then(resolve);
  });
}

function killDaemon(daemon) {
  return new Promise((resolve) => {
    if (!daemon || daemon.killed) {
      resolve();
      return;
    }
    daemon.once('exit', resolve);
    daemon.kill('SIGTERM');
    setTimeout(() => {
      if (!daemon.killed) daemon.kill('SIGKILL');
      resolve();
    }, 1000);
  });
}

function connectApi(port) {
  return new Promise((resolve, reject) => {
    const sock = net.createConnection({ port, host: '127.0.0.1', noDelay: true });
    sock.setEncoding('utf8');
    
    const timeout = setTimeout(() => {
      sock.destroy();
      reject(new Error('Connection timeout'));
    }, TIMEOUT);
    
    sock.on('connect', () => {
      clearTimeout(timeout);
      resolve(sock);
    });
    
    sock.on('error', reject);
  });
}

function connectUnixApi(socketPath) {
  return new Promise((resolve, reject) => {
    const sock = net.createConnection({ path: socketPath, noDelay: true });
    sock.setEncoding('utf8');
    
    const timeout = setTimeout(() => {
      sock.destroy();
      reject(new Error('Connection timeout'));
    }, TIMEOUT);
    
    sock.on('connect', () => {
      clearTimeout(timeout);
      resolve(sock);
    });
    
    sock.on('error', reject);
  });
}

function encodeResp(...args) {
  const n = args.length;
  let cmd = `*${n}\r\n`;
  for (const arg of args) {
    const s = String(arg);
    cmd += `$${s.length}\r\n${s}\r\n`;
  }
  return cmd;
}

function apiCommand(sock, ...args) {
  return new Promise((resolve, reject) => {
    const cmd = encodeResp(...args);
    
    let response = '';
    const timeout = setTimeout(() => {
      sock.destroy();
      reject(new Error('API command timeout'));
    }, TIMEOUT);
    
    sock.once('data', (data) => {
      response += data;
      clearTimeout(timeout);
      resolve(parseResp(response));
    });
    
    sock.write(cmd);
  });
}

function parseResp(data) {
  data = data.trim();
  if (data.startsWith('+')) return data.substring(1).trim();
  if (data.startsWith('-')) throw new Error(data.substring(1).trim());
  if (data.startsWith(':')) return parseInt(data.substring(1), 10);
  if (data.startsWith('*')) {
    const count = parseInt(data.substring(1), 10);
    if (count === 0) return [];
    const lines = data.split('\r\n');
    const result = [];
    let i = 1;
    for (let j = 0; j < count && i < lines.length; j++) {
      if (lines[i].startsWith('$')) {
        i++;
        if (i < lines.length) result.push(lines[i]);
      } else if (lines[i].startsWith(':')) {
        result.push(parseInt(lines[i].substring(1), 10));
      } else if (lines[i].startsWith('+')) {
        result.push(lines[i].substring(1));
      } else if (lines[i].startsWith('-')) {
        throw new Error(lines[i].substring(1));
      }
      i++;
    }
    return result;
  }
  if (data.startsWith('$')) {
    const len = parseInt(data.substring(1), 10);
    if (len === -1) return null;
    const idx = data.indexOf('\r\n');
    if (idx >= 0) return data.substring(idx + 2);
    return '';
  }
  return data;
}

function createUdpEchoServer() {
  return new Promise(async (resolve, reject) => {
    const server = dgram.createSocket('udp4');
    const messages = [];
    
    server.on('message', (msg, rinfo) => {
      messages.push({ data: msg.toString(), rinfo });
      server.send(msg, rinfo.port, rinfo.address);
    });
    
    server.on('error', reject);
    
    const port = await findFreePort();
    server.bind(port, '127.0.0.1', () => {
      resolve({
        port,
        socket: server,
        getMessages: () => messages,
        clearMessages: () => { messages.length = 0; }
      });
    });
  });
}

function sendUdp(port, host, message) {
  return new Promise((resolve, reject) => {
    const sock = dgram.createSocket('udp4');
    const buf = Buffer.from(message);
    sock.send(buf, 0, buf.length, port, host, (err) => {
      sock.close();
      if (err) reject(err);
      else resolve();
    });
  });
}

function recvUdp(port, timeout) {
  return new Promise((resolve, reject) => {
    const sock = dgram.createSocket('udp4');
    sock.bind(port, '127.0.0.1');
    
    const timer = setTimeout(() => {
      sock.close();
      reject(new Error('Receive timeout'));
    }, timeout || TIMEOUT);
    
    sock.on('message', (msg, rinfo) => {
      clearTimeout(timer);
      sock.close();
      resolve({ data: msg.toString(), rinfo });
    });
    
    sock.on('error', (err) => {
      clearTimeout(timer);
      reject(err);
    });
  });
}

module.exports = {
  sleep,
  findFreePort,
  spawnDaemon,
  killDaemon,
  killAllDaemons,
  connectApi,
  connectUnixApi,
  apiCommand,
  createUdpEchoServer,
  sendUdp,
  recvUdp,
  TIMEOUT
};
