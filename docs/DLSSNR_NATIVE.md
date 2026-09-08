# DLSSNR nativo: estado medido em 2026-09-06

A integração é adicional. PyTorch, ONNX, MIGraphX e as camadas DX9/DX11/DX12
continuam disponíveis. O launcher `launch-fc3.sh`, na pasta farcrydata, seleciona
um perfil e um host separados para o modelo nativo 1920x1080/gfx1200.

## Correções da versão 0.1.2

- A captura anterior inicializava o motor, mas não reproduzia a transferência de
  controles feita pelo worker do mod. LocalStructure e SkinStructure chegavam
  zerados à rede. A captura agora aplica LocalTone=0, LocalStructure=1 e
  SkinStructure herdando LocalStructure, conforme a configuração local do mod.
  O empacotador recusa a captura com esses controles ausentes.
- `DLSSNR_NO_REPACK=0` não equivale a desativar a variável: a DLL verifica sua
  presença. Ela foi removida, e o pacote usa o upload real de pesos reorganizados,
  não o arquivo intermediário DLSSNRW1.
- A camada misturava a saída com leituras escalares da memória GPU mapeada,
  inclusive com intensidade 100%. A composição agora ocorre em RAM e volta em
  uma cópia em bloco. O staging prefere memória coerente com cache de CPU;
  pesos e ativações da rede continuam alocados uma vez por sessão com hipMalloc.
- Logs `host64_frame` distinguem cópia CPU, troca com o host, composição e
  inferência. O tempo de inferência sozinho não é apresentado como FPS do jogo.

## Evidência e limites

O teste DX11 de 1920x1080 reproduziu apresentações de aproximadamente 1.2–1.4 s
na camada anterior. Depois da correção, mediu aproximadamente 70–80 ms por
apresentação aquecida, com variações. Isso ainda não comprova 30 FPS no Far Cry 3.
Inicialização e aquecimento são medidos separadamente dos quadros estáveis.

A imagem sintética com textura/ruído passou de alteração média de 0.61 para
4.93 níveis de 8 bits por canal após restaurar os controles. Esse teste comprova
que os controles mudam a saída; não comprova qualidade equivalente ao add-on RTX.
Os buffers persistentes somam 823132988 bytes (pesos e ativações), sem contar o
módulo HIP, o driver e os recursos Vulkan do jogo.

Validações: 9 testes CPU (incluindo composição 0%, 50%, 100% e preservação de
alpha); execução nativa por DX9, DX11 e DX12 em prefixo isolado; DX9 também com
aplicação e camada de 32 bits. Logs e comparações locais ficam em
`build/dlssnr-native/qualification` e `conditioning-check.json`.

## Presets e comparação com dlss5-bridge

Na DLL local SHA256 dcc0dc2414aedec4a8e084647070383be068554042587180c20c784d4772d36f,
a função em VA 0x180023a40 percorre a tabela [0x1800b0d80,0x1800b1008) com
passo 0x288: exatamente um descritor, preset 1, com o nome
`CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04_22_30_weights`.
Um preset ausente retorna ao descritor padrão. Não há evidência de redes X2/X3
separadas nesse arquivo; não foram criados presets fictícios.

O [dlss5-bridge](https://github.com/NIGos/dlss5-bridge), examinado no commit
28aed4099b0fe1c207b20b5fee5364c0773c25c2, encaminha recursos e parâmetros a um
add-on NVIDIA externo. `bridge.inc` distingue PerfQualityValue do DLSS e os
presets de pesos; copia profundidade, movimento, exposição e dimensões da
aplicação. `synth.inc` implementa um contrato aproximado para jogos sem DLSS.
Ele não fornece um executor AMD da rede. Nenhum código desse projeto foi
incorporado ao NeuroShade nesta correção.

O backend atual recebe cor final, sem profundidade/movimento externos. A
paridade visual e de desempenho com a RTX 2060 permanece não comprovada.
O launcher antigo, os modelos anteriores e backups da instalação são mantidos.
