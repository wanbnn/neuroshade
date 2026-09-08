# Host C++ e transporte Vulkan/HIP

O backend nativo possui um host Linux C++ de 64 bits (`ns-dlssnr-host`). A camada
Vulkan de 32 bits continua usando mensagens com inteiros de tamanho fixo; ela
não carrega HIP nem Python. Os hosts PyTorch/ONNX continuam disponíveis.

Com `NEUROSHADE_GPU_SHARED=1`, a camada solicita a extensão de memória externa
quando a GPU oferece também identificação PCI. O host anuncia a capacidade;
modelos/hosts anteriores continuam usando o transporte pela CPU. O endereço PCI
completo deve coincidir entre Vulkan e HIP. Os UUIDs RADV/HIP observados nesta
máquina são diferentes, portanto não são usados como identidade entre drivers.

Cada imagem do swapchain recebe um buffer device-local exportável. O descritor
é enviado uma vez por SCM_RIGHTS e importado uma vez pelo host. Por quadro, o
socket transporta apenas o índice do buffer, formato e intensidade. A conversão
BGRA/RGBA, composição de intensidade e preservação de alfa executam na GPU.
Pesos, ativações e HIP Graph permanecem residentes e reutilizados.

A sincronização desta versão usa uma fence Vulkan antes do acesso HIP,
`hipStreamSynchronize` antes da resposta do host e barreiras de transferência de
propriedade para/de `VK_QUEUE_FAMILY_EXTERNAL`. Os semáforos existentes continuam
ordenando apresentação/jogo. Ainda não há semáforos externos HIP/Vulkan para
retirar essas esperas da CPU; isso não faz pixels passarem pela CPU.

Falhas de capacidade/importação durante a criação selecionam staging por
imagem. Falha de execução em memória compartilhada interrompe a submissão:
não é seguro apresentar uma alocação potencialmente ainda acessada pelo HIP.
O host atende uma conexão ativa de cada vez, com estado recorrente próprio e
liberado ao desconectar. Use uma instância/socket por jogo.

## Validação

`ns-dlssnr-interop MODEL.nsmodel` compara o caminho compartilhado com o caminho
RGBA do runtime em 1920x1080, formatos RGBA/BGRA e intensidades 0, 128/255, 1.
Os seis resultados são comparados byte a byte, inclusive alfa.
DX9, DX11 e DX12 de 64 bits passaram com transporte compartilhado; DX9 de
32 bits passou com 12 apresentações e um reset. No qualificador de 32 bits,
`PeekMessageW` bloqueou antes do primeiro quadro também com a camada desligada;
a qualificação de submissão usou 12 quadros limitados sem essa chamada. Isso
valida a submissão gráfica, não uma sessão completa de jogo. O programa usa
um lock exclusivo de qualificação. Transferências CPU no qualificador servem
apenas para fornecer o estímulo e conferir o resultado.

A implementação anterior de conversão CPU também foi comparada com a nova,
em seis quadros (dois formatos, três estados temporais), com igualdade exata.
O teste de protocolo CPU verifica passagem do FD, sua propriedade/fechamento,
campos de controle e intensidade; não exige GPU.

Na qualificação DX11 local, após aquecimento, apresentação compartilhada ficou
em 47–52 ms por quadro, contra aproximadamente 70–80 ms com staging anterior.
Isso é um programa sintético de apresentação, não FPS medido no Far Cry 3.
O grafo neural continua custando aproximadamente 47 ms: o transporte não resolve
sozinho o orçamento de 33,3 ms necessário para 30 FPS. Pesos e kernels Swin da
rede não foram alterados por esta melhoria.

## Execução

```sh
cmake --build build/dlssnr-native --target ns-dlssnr-host ns-dlssnr-interop
cmake --install build/dlssnr-native --prefix "$HOME/.local" --component DLSSNRNative
ns_host="$HOME/.local/libexec/neuroshade/ns-dlssnr-host"
"$ns_host" --socket "$HOME/.local/state/neuroshade/native/runtime.sock" \
  --model "$HOME/.local/share/neuroshade/models/dlssnr-1080p.nsmodel"
```

O host verifica os quatro SHA256 publicados no pacote antes de carregar o
código GPU. Ele só aceita o modelo configurado na linha de comando e conexões
do mesmo usuário; a entrada de criação de buffer limita quantidade e tamanho.

A instalação local também passou com o serviço C++, modelo 0.1.2, camada x86,
os três efeitos do perfil FC3 e overlay visível. O launcher inicia
`neuroshade-fc3-native.service`; logs do host ficam no journal do usuário.
`NEUROSHADE_GPU_SHARED=0 ./launch-fc3.sh` seleciona o transporte CPU otimizado.
Dados da entrega: [relatório JSON](benchmarks/dlssnr-gpu-transport.json).

Nove testes CPU relevantes passaram. A execução indiscriminada do CTest no
build nativo (camada desabilitada) também tentou dois testes de empacotamento
sem manifesto Vulkan e um teste de shader sem script configurado; esses três
não passaram nessa configuração e não são contabilizados como validação.
