# Template System Guide

Complete guide to gscope's template provisioning system.

## Overview

Templates are JSON configurations that automatically provision scopes by:
1. Installing system packages (apt, pip, npm, cargo, gem)
2. Creating configuration files with variable substitution
3. Running setup and initialization scripts
4. Verifying the installation

## Template JSON Schema

```json
{
  "name": "string — Template display name",
  "version": "string — Semantic version",
  "template_id": "string — Unique identifier",

  "variables": {
    "key": "value — User-defined variables for substitution"
  },

  "environment": {
    "KEY": "VALUE — Environment variables for all commands"
  },

  "packages": [
    {
      "name": "string — Package name",
      "version": "string (optional) — Version constraint",
      "manager": "apt|pip|npm|cargo|gem",
      "required": "boolean (default: true)",
      "pre_install_script": "string (optional) — Run before this package",
      "post_install_script": "string (optional) — Run after this package"
    }
  ],

  "files": [
    {
      "path": "string — Absolute path inside scope",
      "content": "string — File content",
      "type": "template|raw — template = apply variable substitution",
      "mode": "string — Octal permissions (e.g. '0644')"
    }
  ],

  "pre_install_script": "string — Runs before package installation",
  "post_install_script": "string — Runs after all packages",
  "setup_script": "string — Main setup (DB init, config generation)",
  "startup_script": "string — Saved to /opt/gritiva/startup.sh",
  "health_check_script": "string — Saved to /opt/gritiva/health_check.sh",

  "verification": {
    "commands": [{"name": "string", "command": "string — Must exit 0"}],
    "files": [{"path": "string — Must exist"}],
    "ports": [{"port": "number — Must be listening"}]
  }
}
```

## Example Templates

### Python Web Application

```json
{
  "name": "Python Flask App",
  "version": "1.0.0",
  "variables": {
    "project_name": "myapi",
    "port": "5000",
    "db_url": "sqlite:///opt/data/app.db"
  },
  "packages": [
    {"name": "python3-pip", "manager": "apt"},
    {"name": "python3-venv", "manager": "apt"},
    {"name": "flask", "manager": "pip"},
    {"name": "gunicorn", "manager": "pip"}
  ],
  "files": [
    {
      "path": "/opt/app/app.py",
      "type": "template",
      "content": "from flask import Flask\napp = Flask('${project_name}')\n\n@app.route('/')\ndef index():\n    return {'app': '${project_name}', 'status': 'running'}\n\nif __name__ == '__main__':\n    app.run(host='0.0.0.0', port=${port})\n"
    }
  ],
  "pre_install_script": "mkdir -p /opt/app /opt/data",
  "setup_script": "cd /opt/app && python3 -m venv venv && venv/bin/pip install flask gunicorn",
  "startup_script": "cd /opt/app && venv/bin/gunicorn -w 2 -b 0.0.0.0:${port} app:app",
  "verification": {
    "commands": [{"name": "flask", "command": "pip3 show flask"}],
    "files": [{"path": "/opt/app/app.py"}]
  }
}
```

### PostgreSQL Database

```json
{
  "name": "PostgreSQL",
  "version": "1.0.0",
  "variables": {
    "pg_version": "16",
    "pg_port": "5432",
    "pg_user": "appuser",
    "pg_password": "secret",
    "pg_database": "appdb",
    "max_connections": "100"
  },
  "packages": [
    {"name": "postgresql", "manager": "apt"},
    {"name": "postgresql-client", "manager": "apt"}
  ],
  "files": [
    {
      "path": "/etc/postgresql/custom.conf",
      "type": "template",
      "content": "port = ${pg_port}\nmax_connections = ${max_connections}\nlisten_addresses = '*'\nshared_buffers = 128MB\nlog_destination = 'stderr'\nlogging_collector = on\nlog_directory = '${LOG_DIR}'\n"
    }
  ],
  "setup_script": "pg_ctlcluster ${pg_version} main start && su - postgres -c \"psql -c \\\"CREATE USER ${pg_user} WITH PASSWORD '${pg_password}';\\\"\" && su - postgres -c \"psql -c \\\"CREATE DATABASE ${pg_database} OWNER ${pg_user};\\\"\"",
  "startup_script": "pg_ctlcluster ${pg_version} main start",
  "health_check_script": "pg_isready -p ${pg_port}",
  "verification": {
    "commands": [{"name": "pg_running", "command": "pg_isready"}],
    "ports": [{"port": 5432}]
  }
}
```

### Node.js Application

```json
{
  "name": "Node.js Express",
  "variables": {
    "project_name": "myapp",
    "node_version": "20",
    "port": "3000"
  },
  "packages": [
    {"name": "nodejs", "manager": "apt"},
    {"name": "npm", "manager": "apt"},
    {"name": "express", "manager": "npm"},
    {"name": "nodemon", "manager": "npm"}
  ],
  "files": [
    {
      "path": "/opt/app/index.js",
      "type": "template",
      "content": "const express = require('express');\nconst app = express();\n\napp.get('/', (req, res) => res.json({app: '${project_name}', status: 'running'}));\napp.get('/health', (req, res) => res.json({status: 'healthy'}));\n\napp.listen(${port}, () => console.log('${project_name} on port ${port}'));\n"
    },
    {
      "path": "/opt/app/package.json",
      "type": "template",
      "content": "{\"name\": \"${project_name}\", \"version\": \"1.0.0\", \"main\": \"index.js\", \"dependencies\": {\"express\": \"^4.18.0\"}}\n"
    }
  ],
  "setup_script": "cd /opt/app && npm install",
  "startup_script": "cd /opt/app && node index.js",
  "health_check_script": "curl -sf http://localhost:${port}/health"
}
```

### Nginx Reverse Proxy

```json
{
  "name": "Nginx Reverse Proxy",
  "variables": {
    "server_name": "app.example.com",
    "upstream_host": "127.0.0.1",
    "upstream_port": "5000",
    "listen_port": "80"
  },
  "packages": [
    {"name": "nginx", "manager": "apt"}
  ],
  "files": [
    {
      "path": "/etc/nginx/sites-available/default",
      "type": "template",
      "content": "server {\n    listen ${listen_port};\n    server_name ${server_name};\n\n    location / {\n        proxy_pass http://${upstream_host}:${upstream_port};\n        proxy_set_header Host $host;\n        proxy_set_header X-Real-IP $remote_addr;\n    }\n}\n"
    }
  ],
  "startup_script": "nginx -g 'daemon off;'",
  "health_check_script": "curl -sf http://localhost:${listen_port}/",
  "verification": {
    "commands": [{"name": "nginx", "command": "nginx -t"}],
    "ports": [{"port": 80}]
  }
}
```

## Composite Templates

Templates can install multiple technologies in one scope:

```json
{
  "name": "Python + PostgreSQL + Redis",
  "packages": [
    {"name": "python3-pip", "manager": "apt"},
    {"name": "postgresql", "manager": "apt"},
    {"name": "redis-server", "manager": "apt"},
    {"name": "flask", "manager": "pip"},
    {"name": "psycopg2-binary", "manager": "pip"},
    {"name": "redis", "manager": "pip"}
  ],
  "setup_script": "pg_ctlcluster 16 main start && redis-server --daemonize yes && cd /opt/app && python3 -m venv venv && venv/bin/pip install flask psycopg2-binary redis"
}
```

## Progress Callback

The C API provides a progress callback for real-time tracking:

```c
void my_progress(const gscope_tmpl_progress_t *p, void *ud) {
    printf("[%d] %3d%% %s\n", p->phase, p->progress, p->message);
}

gscope_template_execute(scope, tmpl, my_progress, NULL, &result);
```

Output:
```
[0]   0% Running preflight checks...
[0]   5% Preflight checks passed
[1]   8% Setting up variables...
[2]  10% Running pre-install script...
[3]  15% Installing packages...
[3]  20% Installing 3 apt package(s)...
[3]  55% 3 package(s) installed
[5]  65% Creating configuration files...
[5]  75% 3 file(s) created
[6]  78% Running setup script...
[7]  90% Running verification checks...
[7]  95% 5/5 verifications passed
[8] 100% Template execution complete!
```
