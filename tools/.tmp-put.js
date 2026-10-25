const { Client } = require('ssh2');
const fs = require('fs'); const os = require('os'); const path = require('path');
const DEPLOY = require('./deploy-config');
const KEY = path.join(os.homedir(), '.ssh', 'kunkun_admin_ed25519');
const [src, dst] = process.argv.slice(2);
const conn = new Client();
conn.on('ready', () => conn.sftp((e, sftp) => {
  if (e) { console.error(e.message); process.exit(1); }
  sftp.fastPut(src, dst, (err) => { if (err) { console.error(err.message); process.exit(1); } console.log('uploaded', dst); conn.end(); process.exit(0); });
})).on('error', (e) => { console.error('SSH', e.message); process.exit(2); })
  .connect({ host: DEPLOY.host, port: DEPLOY.port, username: DEPLOY.username, ...(fs.existsSync(KEY) ? { privateKey: fs.readFileSync(KEY) } : { password: DEPLOY.password }), readyTimeout: 20000 });
