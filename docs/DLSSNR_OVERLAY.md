# DLSSNR no overlay (Home → F8)

O painel agora oferece NR Style, NR Preset, NR Intensity, mistura da saída,
Automatic Mask, Local Tone, Local Structure e Skin Structure. Clique nos botões
Style/Preset para percorrer as opções; ajuste os valores com −/+. Aplicar atualiza
a execução e Salvar persiste a seleção sem remover outros efeitos do perfil.

- NR Style: Default (0), Natural (1), Cinematic (2). Condicionam a própria rede,
  com o valor style/128 no canal recuperado do kernel pre. Os três produzem
  imagens distintas; não são filtros de cor externos.
- NR Preset: Default (0), #1, #2, #3. O descritor da DLL local tem apenas os pesos
  do #1. Assim como a DLL, as demais escolhas resolvem para #1; o painel mostra
  o preset efetivo e sinaliza o fallback para #2/#3. Não existem três conjuntos
  de pesos distintos neste pacote.
- NR Intensity: 0–2. Escala a alteração neural em FP32 antes da quantização e
  do recorte de faixa na exportação: `entrada + intensity * (rede - entrada)`.
  0 preserva a entrada, 1 mantém o resultado normal e 2 amplia a alteração.
  Esta é a implementação nativa da força do efeito; a fórmula da composição
  NVIDIA, incluindo UI Correction/máscaras externas, não foi comprovada como
  equivalente. O teste não usa uma execução RTX como referência de intensidade.
- Mistura: 0–100%, composição final com o quadro original. É independente da
  intensidade e preserva o controle `strength` dos perfis existentes.
- Local Tone/Structure: 0–2; Skin Structure: -1–2. Pele negativa herda a estrutura
  geral quando Automatic Mask está ligado. Desligá-lo envia -1 para os dois
  canais derivados da rede, seguindo o comportamento recuperado dos binários.

Alterar os controles recompila a instância do HIP Graph e limpa o histórico.
Pesos e buffers compartilhados permanecem residentes. Intensidade diferente de
1 usa um buffer auxiliar persistente de 31,64 MiB em 1080p e um kernel GPU antes
da exportação. O tensor original da rede não é alterado por esse kernel.

Restaurar NR usa Default para Style/Preset, intensidade 1, mistura 100%, tom 0,
estruturas 1 e máscara ligada, mantendo os padrões anteriores.

Campos opcionais do perfil schema 2:

```json
{
  "plugin": "org.neuroshade.reconstruction",
  "enabled": true,
  "model": "models/dlssnr-1080p.nsmodel",
  "strength": 1,
  "nr_style": 2,
  "nr_preset": 0,
  "nr_intensity": 1.38,
  "nr_local_tone": 1.54,
  "nr_local_structure": 1,
  "nr_skin_structure": -1,
  "nr_auto_mask": true
}
```

Ausência dos campos mantém os padrões anteriores. Seleções fracionárias,
fora da faixa, valores não finitos e máscaras não booleanas são rejeitados.
O host anuncia `nr_controls_v3`; a operação 8 recebe sete float32: tom, estrutura,
pele, máscara (0/1), style (0/1/2), preset (0/1/2/3), intensidade (0–2). Operações
6 e 7 continuam disponíveis. O cliente informa a necessidade de reiniciar um
host antigo ao solicitar controles que ele não oferece. O launcher detecta a
substituição do executável e o reinicia antes de iniciar o jogo.

Evidências: [análise do add-on](DLSSNR_ADDON_ANALYSIS.md),
[descritor de modos](benchmarks/dlssnr-mode-descriptor.json) e
[qualificação GPU](benchmarks/dlssnr-modes-v3.json). Os testes cobrem três estilos
distintos, referência do dispatch, fallback de presets, intensidade zero igual
à entrada, níveis 0/0,5/1/1,38/2, kernel FP32 e troca/restauração na mesma sessão.
Isso não constitui uma comparação visual completa com NVIDIA/RTX.
