/** 上传最新 KunClient.exe 到服务器 assets（一次性发布脚本，用完可删） */
const { Client } = require('ssh2');
const fs = require('fs');
const DEPLOY = require('./deploy-config');

const LOCAL = 'E:/flashtool/publish/KunClient.exe';
const REMOTE = `${DEPLOY.remoteBase}/assets/KunClient.exe`;

const conn = new Client();
conn.on('ready', () => {
  conn.sftp((err, sftp) => {
    if (err) { console.error('SFTP error:', err.message); process.exit(1); }
    const rs = fs.createReadStream(LOCAL);
    const ws = sftp.createWriteStream(REMOTE);
    ws.on('error', e => { console.error('Upload fail:', e.message); process.exit(1); });
    ws.on('close', () => {
      sftp.stat(REMOTE, (e, st) => {
        console.log(`Uploaded: ${st ? st.size : '?'} bytes (local ${fs.statSync(LOCAL).size})`);
        conn.end();
      });
    });
    rs.pipe(ws);
  });
}).on('error', e => { console.error('SSH error:', e.message); process.exit(1); })
  .connect({ host: DEPLOY.host, port: DEPLOY.port, username: DEPLOY.username, password: DEPLOY.password });