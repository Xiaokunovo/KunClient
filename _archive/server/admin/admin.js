const API = window.location.origin;
let token = localStorage.getItem('adminToken');
let deleteUserId = null;

document.addEventListener('DOMContentLoaded', function() {
    document.getElementById('loginBtn').addEventListener('click', login);
    document.getElementById('logoutBtn').addEventListener('click', logout);
    document.getElementById('addUserBtn').addEventListener('click', showAddUserModal);
    document.getElementById('closeUserModal').addEventListener('click', () => closeModal('userModal'));
    document.getElementById('cancelUserModal').addEventListener('click', () => closeModal('userModal'));
    document.getElementById('saveUserBtn').addEventListener('click', saveUser);
    document.getElementById('refreshLogsBtn').addEventListener('click', loadLogs);
    document.getElementById('confirmDeleteBtn').addEventListener('click', confirmDelete);
    document.getElementById('loginPassword').addEventListener('keypress', function(e) { if (e.key === 'Enter') login(); });

    if (token) {
        verifyToken().catch(() => { token = null; localStorage.removeItem('adminToken'); });
    }
});

async function login() {
    const username = document.getElementById('loginUsername').value;
    const password = document.getElementById('loginPassword').value;
    const errorEl = document.getElementById('loginError');
    errorEl.style.display = 'none';

    if (!username || !password) { errorEl.textContent = '请输入用户名和密码'; errorEl.style.display = 'block'; return; }

    try {
        const res = await fetch(`${API}/api/login`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ username, password })
        });
        const data = await res.json();
        if (data.success) {
            if (data.data.role !== 'admin') { errorEl.textContent = '需要管理员权限'; errorEl.style.display = 'block'; return; }
            token = data.data.token;
            localStorage.setItem('adminToken', token);
            document.getElementById('currentUser').textContent = username;
            showDashboard();
        } else {
            errorEl.textContent = data.message || '登录失败';
            errorEl.style.display = 'block';
        }
    } catch (e) {
        errorEl.textContent = '连接服务器失败';
        errorEl.style.display = 'block';
    }
}

async function verifyToken() {
    const res = await fetch(`${API}/api/verify`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ token })
    });
    const data = await res.json();
    if (data.success && data.data.role === 'admin') {
        document.getElementById('currentUser').textContent = data.data.username;
        showDashboard();
    } else {
        throw new Error('Invalid token');
    }
}

function showDashboard() {
    document.getElementById('loginPage').style.display = 'none';
    document.getElementById('dashboard').style.display = 'block';
    loadStats();
    loadUsers();
    loadLogs();
}

async function loadStats() {
    try {
        const res = await fetch(`${API}/api/stats`, { headers: { 'Authorization': `Bearer ${token}` } });
        const data = await res.json();
        if (data.success) {
            document.getElementById('statTotal').textContent = data.data.totalUsers;
            document.getElementById('statActive').textContent = data.data.activeUsers;
            document.getElementById('statLogins').textContent = data.data.loginsToday;
            document.getElementById('statFailed').textContent = data.data.failedLogins;
        }
    } catch (e) { console.error('Load stats failed:', e); }
}

async function loadUsers() {
    try {
        const res = await fetch(`${API}/api/users`, { headers: { 'Authorization': `Bearer ${token}` } });
        const data = await res.json();
        if (data.success) {
            document.getElementById('usersTable').innerHTML = data.data.map(u => `
                <tr>
                    <td>${u.id}</td>
                    <td><strong>${u.username}</strong></td>
                    <td><span class="badge badge-info">${u.role === 'admin' ? '管理员' : '用户'}</span></td>
                    <td><span class="badge ${u.status === 'active' ? 'badge-success' : 'badge-danger'}">${u.status === 'active' ? '正常' : '已封禁'}</span></td>
                    <td>${u.created_at || '-'}</td>
                    <td>${u.last_login || '从未登录'}</td>
                    <td>
                        <button class="action-btn edit" onclick="editUser(${u.id},'${u.username}','${u.role}','${u.status}')">编辑</button>
                        <button class="action-btn" style="background:#FF9800;color:white" onclick="showResetPwdModal(${u.id},'${u.username}')">密码</button>
                        <button class="action-btn delete" onclick="showDeleteModal(${u.id},'${u.username}')">删除</button>
                    </td>
                </tr>
            `).join('');
        }
    } catch (e) { console.error('Load users failed:', e); }
}

async function loadLogs() {
    try {
        const res = await fetch(`${API}/api/logs?limit=50`, { headers: { 'Authorization': `Bearer ${token}` } });
        const data = await res.json();
        if (data.success) {
            document.getElementById('logsTable').innerHTML = data.data.map(log => `
                <tr>
                    <td>${log.login_time}</td>
                    <td>${log.username}</td>
                    <td><span class="badge ${log.status === 'success' ? 'badge-success' : 'badge-danger'}">${log.status === 'success' ? '成功' : '失败'}</span></td>
                </tr>
            `).join('') || '<tr><td colspan="3" style="text-align:center;color:#999">暂无日志</td></tr>';
        }
    } catch (e) { console.error('Load logs failed:', e); }
}

function showAddUserModal() {
    document.getElementById('modalTitle').textContent = '添加用户';
    document.getElementById('editUserId').value = '';
    document.getElementById('modalUsername').value = '';
    document.getElementById('modalPassword').value = '';
    document.getElementById('modalRole').value = 'user';
    document.getElementById('modalStatus').value = 'active';
    document.getElementById('pwdGroup').style.display = 'block';
    openModal('userModal');
}

function editUser(id, username, role, status) {
    document.getElementById('modalTitle').textContent = '编辑用户';
    document.getElementById('editUserId').value = id;
    document.getElementById('modalUsername').value = username;
    document.getElementById('modalPassword').value = '';
    document.getElementById('modalRole').value = role;
    document.getElementById('modalStatus').value = status;
    document.getElementById('pwdGroup').style.display = 'none';
    openModal('userModal');
}

async function saveUser() {
    const id = document.getElementById('editUserId').value;
    const username = document.getElementById('modalUsername').value;
    const password = document.getElementById('modalPassword').value;
    const role = document.getElementById('modalRole').value;
    const status = document.getElementById('modalStatus').value;

    if (!username) { alert('请输入用户名'); return; }

    try {
        let res;
        if (id) {
            res = await fetch(`${API}/api/users/${id}`, {
                method: 'PUT',
                headers: { 'Content-Type': 'application/json', 'Authorization': `Bearer ${token}` },
                body: JSON.stringify({ username, role, status })
            });
        } else {
            if (!password) { alert('请输入密码'); return; }
            res = await fetch(`${API}/api/users`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json', 'Authorization': `Bearer ${token}` },
                body: JSON.stringify({ username, password, role })
            });
        }
        const data = await res.json();
        if (data.success) { closeModal('userModal'); loadUsers(); loadStats(); }
        else { alert(data.message || '操作失败'); }
    } catch (e) { alert('操作失败'); }
}

function showDeleteModal(id, username) {
    deleteUserId = id;
    document.getElementById('deleteUserName').textContent = `用户: ${username}`;
    openModal('deleteModal');
}

async function confirmDelete() {
    if (!deleteUserId) return;
    try {
        const res = await fetch(`${API}/api/users/${deleteUserId}`, {
            method: 'DELETE',
            headers: { 'Authorization': `Bearer ${token}` }
        });
        const data = await res.json();
        if (data.success) { closeModal('deleteModal'); loadUsers(); loadStats(); }
        else { alert(data.message || '删除失败'); }
    } catch (e) { alert('删除失败'); }
    deleteUserId = null;
}

function showResetPwdModal(userId, username) {
    document.getElementById('resetPwdUserId').value = userId;
    document.getElementById('resetPwdUsername').value = username;
    document.getElementById('resetPwdNew').value = '';
    document.getElementById('resetPwdConfirm').value = '';
    document.getElementById('resetPwdError').style.display = 'none';
    openModal('resetPwdModal');
    document.getElementById('resetPwdNew').focus();
}

async function saveResetPwd() {
    const userId = document.getElementById('resetPwdUserId').value;
    const newPwd = document.getElementById('resetPwdNew').value;
    const confirmPwd = document.getElementById('resetPwdConfirm').value;
    const errorEl = document.getElementById('resetPwdError');
    errorEl.style.display = 'none';

    if (!newPwd) { errorEl.textContent = '请输入新密码'; errorEl.style.display = 'block'; return; }
    if (newPwd.length < 6) { errorEl.textContent = '密码至少需要6位'; errorEl.style.display = 'block'; return; }
    if (newPwd !== confirmPwd) { errorEl.textContent = '两次输入的密码不一致'; errorEl.style.display = 'block'; return; }

    try {
        const res = await fetch(`${API}/api/users/${userId}/reset-password`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json', 'Authorization': `Bearer ${token}` },
            body: JSON.stringify({ newPassword: newPwd })
        });
        const data = await res.json();
        if (data.success) { closeModal('resetPwdModal'); alert('密码已重置'); }
        else { errorEl.textContent = data.message || '重置失败'; errorEl.style.display = 'block'; }
    } catch (e) { errorEl.textContent = '请求失败'; errorEl.style.display = 'block'; }
}

function openModal(id) { document.getElementById(id).classList.add('active'); }
function closeModal(id) { document.getElementById(id).classList.remove('active'); }

function logout() {
    token = null;
    localStorage.removeItem('adminToken');
    document.getElementById('loginPage').style.display = 'flex';
    document.getElementById('dashboard').style.display = 'none';
}
