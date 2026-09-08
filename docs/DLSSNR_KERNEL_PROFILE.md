# Perfil por kernel — RX 9060 XT / gfx1200, 1920×1080

Medição local do pacote nativo 0.1.2, com cinco quadros após cinco aquecimentos.
O novo executável `ns-dlssnr-profile` é C++ nativo e não importa Python/PyTorch.
Ele adquire o lock de qualificação e mede a execução com eventos da GPU.

| Medida | Média por quadro |
|---|---:|
| HIP Graph normal, sem transporte dos pixels pela CPU | 47,065 ms |
| Grafo instrumentado | 48,792 ms |
| Acréscimo da instrumentação | 3,67% |
| Cópias internas device-to-device | 0,201 ms |

Os marcadores de início/fim de cada operação são capturados como nós de eventos
no grafo, evitando uma sincronização CPU/GPU após cada kernel. As sequências de
histórico/aquecimento são iguais nos dois casos. Os cinco resultados completos
foram comparados byte a byte: a instrumentação não mudou a saída.

## Custo acumulado por família

| Família | Chamadas/quadro | ms/quadro |
|---|---:|---:|
| Swin variável 32, variante true | 3 | 14,811 |
| Swin variável 32, variante false | 7 | 6,870 |
| Swin variável 256 | 16 | 5,380 |
| Swin variável 64 | 8 | 4,984 |
| Swin variável 128 | 12 | 4,840 |
| QKV attention 2 | 16 | 2,620 |
| Contract 2 | 16 | 1,803 |
| Conv residual 2 | 31 | 1,766 |

Há 156 chamadas de kernel e quatro cópias internas por quadro. Swin soma cerca
de 36,9 ms. Os dois maiores alvos individuais são o despacho 158 (bloco final,
7,184 ms) e o despacho 1 (bloco inicial, 6,261 ms). Ambos usam
`k_swin_var<32,true>`, grid 240×144, blocos de 256 threads e 15.616 bytes de memória
compartilhada estática. Juntos custam aproximadamente 13,4 ms.

Esses dados apontam onde otimizar, mas não distinguem sozinhos limitação de
banda, cache, ocupação ou cálculo. Não autorizam mudar tamanho dos blocos ou
reordenar pesos sem verificar o resultado. Uma próxima implementação precisa
ser comparada com a saída de referência e com o grafo normal, sem marcadores.

## Reprodução

```sh
cmake -S . -B build/profile -DNS_BUILD_DLSSNR_HIP=ON \
  -DNS_BUILD_HIP_INTEROP=OFF -DNS_BUILD_NEURAL=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/profile --target ns-dlssnr-profile -j2
timeout --kill-after=5s 30s build/profile/bin/ns-dlssnr-profile \
  "$HOME/.local/share/neuroshade/models/dlssnr-1080p.nsmodel" kernel-profile.csv
python3 tools/dlssnr/summarize_profile.py kernel-profile.csv --output kernel-profile.json
```

O Python do último comando apenas agrega o CSV; ele não executa nem cronometra a
rede. Feche o jogo antes da qualificação. O lock evita concorrência entre os
qualificadores, mas não bloqueia jogos iniciados fora deles.

Dados completos: [CSV](benchmarks/dlssnr-gfx1200-1080p.csv) e
[JSON com todos os despachos](benchmarks/dlssnr-gfx1200-1080p.json).
Os tempos são do estímulo sintético determinístico usado pelo profiler, não um
benchmark de FPS do Far Cry 3. Transferências host/device, sockets e apresentação
Vulkan ficam fora do intervalo GPU do grafo.
