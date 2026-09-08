# RenoDX DLSS5: contrato do painel e chamadas NGX

Análise estática do `renodx-dlss5.addon64` fornecido no workspace em 2026-09-07.
PE x64, SHA256 `e1c28fde0922b12fc10734e58c3d24a36808e575247f4fd4f36226540d7ee023`.
Os endereços abaixo são VAs com image base 0x180000000. Evidência local bruta:
`analysis/dlssnr/addon/{strings.txt,disassembly.asm}` no workspace pai.
Não é necessário executar o add-on para recuperar estes contratos.

| Controle | Opções/faixa do add-on | Chamada/campo NGX | VA da chamada |
|---|---|---|---|
| NR Preset | Default=0, Preset #1=1, #2=2, #3=3 | DLSSNR.Hint.Render.Preset, inteiro sem remapeamento | 18001a927 |
| NR Style | Default=0, Natural=1, Cinematic=2 | DLSSNR.Style, inteiro sem remapeamento | 180029d5c |
| NR Intensity | 0–2 | DLSSNR.Intensity, float | 180029cc2 |
| Local Tone Strength | 0–2 | DLSSNR.LocalToneStrength, float | 180029ce1 |
| Local Structure Strength | 0–2 | DLSSNR.LocalStructureStrength, float | 180029d00 |
| Skin Structure Strength | -1–2 | DLSSNR.SkinStructureStrength, float | 180029d1f |
| Automatic Mask | booleano | DLSSNR.UseAutoMask | 180029d44 |
| NR UI Correction | booleano | DLSSNR.UICorrection | 180029d80 |

As faixas não foram inferidas de valores de configuração. Os sliders em
1800211b6/180021223/180021290/180021302 usam 2.0f (constante 180069138),
com mínimo zero ou -1.0f (18006916c para pele). A validação de configuração
em 18001f127 também limita essas faixas. Os combos em 180020fca e 18002104c
passam contagens 4 e 3, respectivamente.

Outros controles encontrados: transferência de cor compatível com Control,
Scene Paper-White Scale, HDR Transfer Strength, Color Strength, Depth Convention
(flag do jogo/normal/invertida), multiplicadores MotionScaleX/Y, Enable Upscaling
(WIP), reset da feature e captura de tela. Nomes existentes no painel não provam
que todos estejam ligados ao grafo HIP recuperado.

## Automatic Mask e pele: implementação qualificada

Na DLL NVIDIA `nvngx_dlssnr.dll` SHA256
`dcc0dc2414aedec4a8e084647070383be068554042587180c20c784d4772d36f`,
18001aa4b em diante transforma os controles:

- Uma máscara externa em objeto+60 desativa o modo automático.
- Automático ligado: derivado de pele = pele se >=0, senão estrutura;
  derivado de estrutura = estrutura.
- Automático desligado: ambos os derivados = -1.0f (1800afc40).

O companion AMD `version.dll` SHA256
`d1f10165de2a328ac2962002c2775af0d8ad9c10030d60197ff53fd4783cff4c`
repete essa transformação em 18000cd20–18000cd64, escrevendo engine+28/+2c.
A constante negativa vem de 180056994, carregada em xmm15 em 18000c8c8.
Capturas do frame executor qualificam a transferência para offsets 96/100
no VarParams de 168 bytes do kernel pre `k_swin_var<32,true>`, flags 20.
Tom e estrutura continuam nos offsets 84/88. No código GPU, os parâmetros são
carregados em 043bd4/043bdc e convertidos para half em 043c38–043c44.

NeuroShade implementa esse condicionamento no grafo GPU, incluindo os valores
negativos. Não utiliza uma máscara heurística externa nem requer um novo upload
de pesos. O pacote atual continua sem entradas de movimento/profundidade externas.

`tools/dlssnr/qualify_controls.py` compara três quadros consecutivos da API com
os grafos capturados do companion para tom=1.54, estrutura=2, pele=-1 e máscara
ligada/desligada. Verifica também diferença efetiva de pixels entre modos,
herança de pele e rejeição de parâmetros inválidos sem alterar a configuração.
Resultado em `docs/benchmarks/dlssnr-controls-v2.json`. Isso qualifica o caminho
HIP recuperado; não é uma comparação de imagens com uma execução NVIDIA/RTX.

## NR Style e Preset integrados

A contagem de estilos é 3, lida em descritor estático 1800b0d80+24. Em
180021086–18002108e, `1800239a0` procura esse descritor pelo nome do peso.
180021235 copia descritor+24 para o temporário em rbp+14 (offset 64 relativamente
a rbp-50), que é movido para o vetor de registros de stride f0. Portanto, o
campo record+64 usado em 1800224e2 é comprovadamente a contagem 3.

O estilo é limitado a essa contagem e escalado por 1/128. A sequência seguinte
18003f490 → 180061710 grava o float no objeto interno+94, lido em 180060ef8
para preparar o dispatch. No executor HIP recuperado, o canal correspondente
é VarParams+92: ele é carregado em s23 por 043bdc, convertido para half em
043c34 e empacotado junto do tom em 043c84. O companion fixa zero nesse campo
em 18001e44c. A captura de qualificação substitui apenas essa constante por
style/128, dentro do processo CPU isolado; não modifica a DLL do jogo.

A API nativa com estilos 1/2 produziu pixels idênticos aos planos desse dispatch
por três quadros. Default/Natural/Cinematic produziram três hashes distintos.

NR Preset tem quatro escolhas no add-on. A tabela da DLL em
[1800b0d80,1800b1008), stride 0x288, contém uma única entrada de pesos: preset 1.
O lookup 180023a40 procura a seleção, depois procura 1 quando ela não existe.
NeuroShade reproduz a resolução e mostra o preset efetivo no overlay. #2/#3 não
passaram a representar modelos distintos.

`tools/dlssnr/inspect_nr_modes.py` verifica o SHA256 da DLL e extrai o descritor.
Resultado: `docs/benchmarks/dlssnr-mode-descriptor.json`.

## NR Intensity: implementação nativa e limite da equivalência

O add-on envia 0–2. A DLL lê o valor em objeto+e0 e o transfere, por exemplo,
para bloco+68 em 18001d420. Existem decisões condicionais de composição quando
intensidade <1, máscara externa ou correção de UI estão presentes. Isso não
prova por si só extrapolação linear acima de 1 na implementação NVIDIA.

Para atender ao controle de força de alteração, NeuroShade aplica
`entrada + intensidade * (rede - entrada)` em FP32, antes da exportação RGBA8.
A entrada recuperada tem três floats por pixel; a saída interna tem quatro.
O exportador qualificado usa formato 1, transferência 0, residual 0 e exposição
1. O novo kernel escreve um buffer auxiliar; não muda o tensor usado pela rede.
O exportador original faz o recorte/quantização final e o compositor preserva
alpha. O controle antigo strength permanece como mistura final independente.

A validação prova intensidade zero igual à entrada, cinco níveis com resultados
distintos, a fórmula FP32 em dados sintéticos incluindo valores fora de [0,1] e
restauração após mudar parâmetros na mesma sessão. Não prova equivalência byte a
byte com a composição NVIDIA/RTX. A intensidade é uma implementação nativa da
força do efeito, e não uma fórmula extraída integralmente dessa DLL.

Continuam fora desta integração: UI Correction, máscaras externas, transferências
HDR e recursos de profundidade/movimento fornecidos pelo jogo.
