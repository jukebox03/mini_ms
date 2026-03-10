#!/bin/bash

NAMESPACE="mini-ms-dpumesh"
YAML_FILE="k8s/mini-ms-dpumesh.yaml"

echo "=== [1/5] Building DPUmesh Images ==="
docker build -t mini-ms-frontend-dpumesh:latest -f frontend/Dockerfile.dpumesh .
docker build -t mini-ms-id-service-dpumesh:latest -f id_service/Dockerfile.dpumesh .
docker build -t mini-ms-attend-service-dpumesh:latest -f attend_service/Dockerfile.dpumesh .

echo "=== [2/5] Importing Images to Containerd ==="
docker save mini-ms-frontend-dpumesh:latest | sudo ctr -n k8s.io images import -
docker save mini-ms-id-service-dpumesh:latest | sudo ctr -n k8s.io images import -
docker save mini-ms-attend-service-dpumesh:latest | sudo ctr -n k8s.io images import -

echo "=== [3/5] Cleaning up existing Workers (Ensuring clean SHM) ==="
kubectl -n $NAMESPACE delete deployment frontend-worker id-service-worker attend-service-worker --ignore-not-found

echo "=== [4/5] Applying Kubernetes Resources ==="
kubectl apply -f $YAML_FILE

echo "=== [5/5] Waiting for DPA/DPU Daemons to initialize SHM ==="
kubectl -n $NAMESPACE wait --for=condition=ready pod -l app=dpa-daemon --timeout=60s
kubectl -n $NAMESPACE wait --for=condition=ready pod -l app=dpu-daemon --timeout=60s

sleep 2

echo "=== Deployment Complete! ==="
echo "All daemons are ready. Workers are being started by Kubernetes."
kubectl -n $NAMESPACE get pods
