# Référence : anatomie détaillée de la jungle de Summoner's Rift

Document de référence pour le blueprint MOBA. Coordonnées normalisées dans
le repère du blueprint : origine en haut-gauche, u vers la droite, v vers le
bas, base bleue en bas-gauche (.11,.86), base rouge en haut-droite (.89,.14).
SR partage ce cadre : mid en diagonale montante, rivière en diagonale
descendante, donc les positions se transposent directement.

## 1. Cadre global

- Carré jouable d'environ 14 800 unités de côté.
- Trois lanes : top et bot longent les bords, mid coupe en diagonale.
- La rivière coupe l'autre diagonale, largeur 4 à 6 % de la map, avec deux
  renflements majeurs : les fosses du Baron (haut-gauche) et du dragon
  (bas-droite), ancrées dans le mur de jungle et ouvertes UNIQUEMENT sur la
  rivière.
- Quatre quadrants de jungle. Chaque camp existe en double par symétrie
  centrale (rotation de 180° autour du centre).

## 2. Positions des camps (normalisées, précision ~±2 %)

Quadrant OUEST (topside bleu) :

| camp   | u    | v    | rôle |
|--------|------|------|------|
| gromp  | .14  | .43  | terrasse en cul-de-sac au coin top-lane/rivière |
| blue   | .26  | .47  | grand camp buff, hub du quadrant |
| wolves | .255 | .565 | camp moyen côté mid |

Quadrant SUD (botside bleu) :

| camp    | u    | v    | rôle |
|---------|------|------|------|
| raptors | .47  | .635 | camp moyen collé à la mid |
| red     | .53  | .73  | grand camp buff, hub du quadrant |
| krugs   | .565 | .82  | camp en cul-de-sac dans le coin bot |

EST = miroir de OUEST, NORD = miroir de SUD ((u,v) -> (1-u,1-v)).

Distances centre à centre : gromp-blue ~.12, blue-wolves ~.10,
raptors-red ~.11, red-krugs ~.095.

**Propriété clé n°1 : les trois camps d'un quadrant ne sont PAS alignés.**
Ils forment un L dont le coude est le camp buff : le virage
gromp->blue->wolves fait environ 75 à 90 degrés (idem raptors->red->krugs).
Le buff est le pivot central du quadrant; les deux petits camps s'écartent
vers des bords différents (l'un vers la lane extérieure ou la rivière,
l'autre vers la mid).

**Propriété clé n°2 : profondeurs étagées.** Gromp et krugs touchent
presque le bord (lane/coin), les buffs sont au centre du quadrant, wolves
et raptors sont à mi-chemin côté mid. Aucun camp n'est à la même distance
des lanes qu'un autre.

## 3. Tailles des clairières (diamètre en % du côté de map)

| camp | diamètre | forme |
|------|----------|-------|
| blue / red | 7-8 % | ovale/haricot, le plus grand |
| krugs | 5.5-6 % | poche de coin |
| wolves | 5.5 % | ovale |
| raptors | 5 % | ronde |
| gromp | 4-4.5 % | petite terrasse |

Les tailles VARIENT : les petits camps font 60-70 % du diamètre des buffs.
Une taille unique de salle est un des marqueurs les plus visibles de
« généré procéduralement ».

## 4. Murs

- Épaisseur typique entre deux espaces jouables : 1.5 à 3.5 % seulement.
  Le mur de SR est une COQUILLE qui épouse les salles et les couloirs, pas
  un anneau massif autonome.
- Les gros volumes de mur (5-8 %) n'existent qu'en bordure de lanes et
  autour des fosses.
- Beaucoup de murs séparent deux couloirs PARALLÈLES (double couloir avec
  cloison fine) : c'est ce qui crée les mind games et les esquives de
  vision.

## 5. Portes et graphe de circulation (ex. topside bleu)

Chaque quadrant a 5 à 7 portes :
- 2 sur la lane extérieure (une près de la base, une près de la rivière),
- 2 sur la mid (une près de la base, une près de la rivière),
- 1 à 2 sur la rivière (dont l'entrée « banane » près du buff).

Le graphe interne n'est PAS une ligne : c'est un petit arbre avec boucles
centré sur le buff :
- blue est un hub à 3-4 bouches (vers gromp, vers wolves, vers la rivière,
  vers la lane côté base);
- gromp est un spur : cul-de-sac à 1-2 bouches (lane + rivière), on y
  entre et on en ressort;
- wolves a 2-3 bouches (vers blue, vers mid, parfois vers la base).

Le full clear n'est donc pas un couloir traversant : c'est
« entrer -> nettoyer -> ressortir par une AUTRE bouche du hub », avec des
allers-retours courts sur les spurs.

Largeur des couloirs : 2.5 à 3.5 %. Les clairières font 1.5 à 2.5 fois la
largeur du couloir qui les dessert.

## 6. Rivière et fosses

- Fosses : 8-9 % de diamètre, mur plein côté jungle, bouche unique côté
  rivière (déjà conforme chez nous).
- Deux entrées de rivière par quadrant, qui débouchent sur les carrefours
  les plus disputés de la map (scuttle).

## 7. Végétation et buissons

- Une trentaine de patchs de buissons : aux portes de jungle, dans la
  rivière, le long des lanes. Les arbres denses vivent SUR les murs; le sol
  jouable reste lisible.

## 8. Écarts actuels du blueprint et corrections à faire

1. **Salles trop grandes, taille unique** : rayon .042 uniforme (8.4 % de
   diamètre) et anneau épais (.024 de demi-largeur, enclos total ~19 %).
   Cible : buffs .033-.036, moyens .026-.028, spurs .020-.022, coquilles
   de mur de .010-.014 de demi-largeur. L'enclos total d'un buff doit
   retomber vers 10 %, celui d'un petit camp vers 7 %.
2. **Camps en ligne** : nos trois camps suivent une même polyline avec un
   zigzag doux. Cible : adopter les positions SR du tableau ci-dessus
   (elles rentrent telles quelles dans nos quadrants), route en L avec le
   buff au coude, gromp/krugs en spurs hors route.
3. **Invariant de test à faire évoluer** : « chaque camp est un sommet de
   la clear route » devient « le buff et le camp moyen sont des sommets;
   les spurs (gromp/krugs) sont reliés au hub par un couloir court en
   cul-de-sac ». Le full clear sans toucher les lanes reste l'invariant
   maître.
4. **Portes** : passer de 2-3 à 5-6 par quadrant (les entrées lane près
   des bases manquent).
5. **Doubles couloirs** : au moins un mur-cloison fin par quadrant entre
   deux couloirs parallèles.

Sources : minimap officielle SR (mesures directes), wiki LoL
(https://wiki.leagueoflegends.com/en-us/Summoner's_Rift), dev blog Riot sur
la refonte de SR, guides de pathing (Dot Esports, Dignitas), positions de
camps issues des données de jeu communautaires (précision ±2 %).
