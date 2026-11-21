#!/bin/sh

cat > "$1" <<'EOF'
#pragma once

#define GENERATED_MESSAGE "Hello generated header example!"
EOF
