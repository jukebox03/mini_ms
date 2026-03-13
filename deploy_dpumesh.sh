#!/bin/bash

NAMESPACE="mini-ms-dpumesh"
YAML_FILE="k8s/mini-ms-dpumesh.yaml"
SHM_PREFIX="minims"

echo "=== [1/8] Building DPUmesh Images ==="
docker build -t mini-ms-frontend-dpumesh:latest -f frontend/Dockerfile.dpumesh .
docker build -t mini-ms-id-service-dpumesh:latest -f id_service/Dockerfile.dpumesh .
docker build -t mini-ms-attend-service-dpumesh:latest -f attend_service/Dockerfile.dpumesh .

echo "=== [2/8] Importing Images to Containerd ==="
docker save mini-ms-frontend-dpumesh:latest | sudo ctr -n k8s.io images import -
docker save mini-ms-id-service-dpumesh:latest | sudo ctr -n k8s.io images import -
docker save mini-ms-attend-service-dpumesh:latest | sudo ctr -n k8s.io images import -

echo "=== [3/8] Cleaning up existing resources ==="
kubectl -n $NAMESPACE delete deployment frontend-worker id-service-worker attend-service-worker --ignore-not-found
kubectl -n $NAMESPACE delete daemonset dpa-daemon dpu-daemon --ignore-not-found
sleep 3

echo "=== [4/8] Cleaning up stale SHM files ==="
sudo rm -f /dev/shm/${SHM_PREFIX}_*
echo "Removed /dev/shm/${SHM_PREFIX}_* files"

echo "=== [5/8] Applying Namespace + DaemonSets (dpa/dpu daemons only) ==="
kubectl apply -f $YAML_FILE -l 'app in (dpa-daemon,dpu-daemon)' 2>/dev/null
# Namespace and DaemonSets may not have labels, so apply full file but delete workers immediately
kubectl apply -f $YAML_FILE
kubectl -n $NAMESPACE delete deployment frontend-worker id-service-worker attend-service-worker --ignore-not-found --wait=false

echo "=== [6/8] Waiting for DPA/DPU Daemons to initialize SHM ==="
kubectl -n $NAMESPACE wait --for=condition=ready pod -l app=dpa-daemon --timeout=60s
kubectl -n $NAMESPACE wait --for=condition=ready pod -l app=dpu-daemon --timeout=60s
sleep 2

echo "=== [7/8] Starting Workers (after SHM is ready) ==="
kubectl apply -f $YAML_FILE

echo "=== [8/8] Waiting for Workers to register ==="
kubectl -n $NAMESPACE wait --for=condition=ready pod -l app=frontend-worker --timeout=60s
kubectl -n $NAMESPACE wait --for=condition=ready pod -l app=id-service-worker --timeout=60s
kubectl -n $NAMESPACE wait --for=condition=ready pod -l app=attend-service-worker --timeout=60s

echo "=== Deployment Complete! ==="
echo "All workers registered. System is ready."
kubectl -n $NAMESPACE get pods
