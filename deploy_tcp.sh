#!/bin/bash

NAMESPACE="mini-ms"
YAML_FILE="k8s/mini-ms.yaml"

echo "=== [1/4] Building TCP Images ==="
docker build -t mini-ms-frontend:latest -f frontend/Dockerfile .
docker build -t mini-ms-id-service:latest -f id_service/Dockerfile .
docker build -t mini-ms-attend-service:latest -f attend_service/Dockerfile .

echo "=== [2/4] Importing Images to Containerd ==="
docker save mini-ms-frontend:latest | sudo ctr -n k8s.io images import -
docker save mini-ms-id-service:latest | sudo ctr -n k8s.io images import -
docker save mini-ms-attend-service:latest | sudo ctr -n k8s.io images import -

echo "=== [3/5] Applying Kubernetes Resources ==="
kubectl apply -f $YAML_FILE

echo "=== [4/5] Restarting Deployments ==="
kubectl -n $NAMESPACE rollout restart deployment frontend id-service attend-service

echo "=== [5/5] Waiting for Pods ==="
kubectl -n $NAMESPACE wait --for=condition=ready pod -l app=frontend --timeout=60s
kubectl -n $NAMESPACE wait --for=condition=ready pod -l app=id-service --timeout=60s
kubectl -n $NAMESPACE wait --for=condition=ready pod -l app=attend-service --timeout=60s

echo "=== Deployment Complete! ==="
kubectl -n $NAMESPACE get pods
